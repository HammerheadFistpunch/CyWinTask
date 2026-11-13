using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace CyWinTask;

public sealed record ProcessSample(int Id, long Created, string Name, int ParentId, double Cpu, long WorkingSet, int Threads, int Handles);
public sealed record Snapshot(List<ProcessSample> Processes, double? Cpu, uint MemoryPercent, double UsedGb, double TotalGb, double CollectionMs);

// Windows x64 SYSTEM_PROCESS_INFORMATION. No process handles or WMI queries in the sampling loop.
public sealed class ProcessSampler : IDisposable
{
    private IntPtr buffer;
    private int capacity = 1024 * 1024;
    private Dictionary<(int, long), long> previous = new();
    private long timestamp;
    private ulong oldIdle, oldTotal;
    private bool hasSystemSample;

    public ProcessSampler()
    {
        if (IntPtr.Size != 8) throw new PlatformNotSupportedException("Sampling requires Windows x64.");
        buffer = Marshal.AllocHGlobal(capacity);
    }

    public Snapshot Collect()
    {
        var watch = Stopwatch.StartNew();
        int length;
        while (true)
        {
            int result = NtQuerySystemInformation(5, buffer, capacity, out length);
            if (result == unchecked((int)0xC0000004))
            {
                int next = Math.Max(checked(capacity * 2), length);
                if (next > 128 * 1024 * 1024) throw new InvalidOperationException("System snapshot too large.");
                buffer = Marshal.ReAllocHGlobal(buffer, (IntPtr)next);
                capacity = next;
                continue;
            }
            if (result < 0) throw new InvalidOperationException($"System snapshot unavailable (NTSTATUS {result:X8}).");
            break;
        }
        long now = Stopwatch.GetTimestamp();
        double elapsed = timestamp == 0 ? 0 : (now - timestamp) / (double)Stopwatch.Frequency;
        var current = new Dictionary<(int, long), long>(previous.Count + 32);
        var rows = new List<ProcessSample>(previous.Count + 32);
        for (int offset = 0; ;)
        {
            if (offset < 0 || offset > length - 256) throw new InvalidOperationException("Unrecognized system snapshot format.");
            IntPtr p = IntPtr.Add(buffer, offset);
            int next = Marshal.ReadInt32(p);
            int id = checked((int)Marshal.ReadInt64(p, 80));
            long created = Marshal.ReadInt64(p, 32);
            long time = Marshal.ReadInt64(p, 40) + Marshal.ReadInt64(p, 48);
            if (id != 0)
            {
                ushort nameBytes = unchecked((ushort)Marshal.ReadInt16(p, 56));
                IntPtr namePtr = Marshal.ReadIntPtr(p, 64);
                long nameOffset = namePtr.ToInt64() - buffer.ToInt64();
                if (nameBytes > 0 && (nameOffset < 0 || nameOffset > length - nameBytes || nameBytes % 2 != 0))
                    throw new InvalidOperationException("Invalid process name in snapshot.");
                string name = nameBytes == 0 ? $"Processes {id}" : Marshal.PtrToStringUni(namePtr, nameBytes / 2)!;
                double cpu = elapsed > 0 && previous.TryGetValue((id, created), out long old)
                    ? Math.Clamp((time - old) / 10000000d / elapsed / Environment.ProcessorCount * 100, 0, 100) : 0;
                current[(id, created)] = time;
                rows.Add(new(id, created, name, checked((int)Marshal.ReadInt64(p, 88)), Math.Round(cpu, 1),
                    Marshal.ReadInt64(p, 144), Marshal.ReadInt32(p, 4), Marshal.ReadInt32(p, 96)));
            }
            if (next == 0) break;
            if (next < 256 || next > length - offset) throw new InvalidOperationException("Invalid process record offset.");
            offset += next;
        }
        timestamp = now;
        previous = current;
        double? systemCpu = null;
        if (GetSystemTimes(out ulong idle, out ulong kernel, out ulong user))
        {
            ulong total = kernel + user;
            if (hasSystemSample && total > oldTotal)
                systemCpu = Math.Clamp(100d * (1 - (idle - oldIdle) / (double)(total - oldTotal)), 0, 100);
            oldIdle = idle; oldTotal = total; hasSystemSample = true;
        }
        var memory = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
        if (!GlobalMemoryStatusEx(ref memory)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return new(rows, systemCpu, memory.Load, (memory.TotalPhysical - memory.AvailablePhysical) / 1073741824d,
            memory.TotalPhysical / 1073741824d, watch.Elapsed.TotalMilliseconds);
    }

    public void Dispose() { if (buffer != IntPtr.Zero) { Marshal.FreeHGlobal(buffer); buffer = IntPtr.Zero; } }
    [DllImport("ntdll.dll")] private static extern int NtQuerySystemInformation(int kind, IntPtr data, int size, out int returned);
    [DllImport("kernel32.dll")] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
    [StructLayout(LayoutKind.Sequential)] private struct MemoryStatus
    {
        public uint Length, Load;
        public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile, TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
    }
}

public static class ProcessActions
{
    private static SafeProcessHandle Open(ProcessSample sample, uint access)
    {
        var handle = OpenProcess(access | 0x1000, false, sample.Id);
        if (handle.IsInvalid) { handle.Dispose(); throw new Win32Exception(Marshal.GetLastWin32Error()); }
        try
        {
            if (!GetProcessTimes(handle, out long created, out _, out _, out _)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (created != sample.Created) throw new InvalidOperationException("The process exited or its PID was reused.");
            return handle;
        }
        catch { handle.Dispose(); throw; }
    }
    public static void Terminate(ProcessSample sample)
    {
        if (sample.Id <= 4 || sample.Id == Environment.ProcessId) throw new InvalidOperationException("CyWinTask cannot end this process.");
        using var handle = Open(sample, 0x1);
        if (!IsProcessCritical(handle, out bool critical)) throw new Win32Exception(Marshal.GetLastWin32Error());
        if (critical) throw new InvalidOperationException("Cannot end a critical Windows process.");
        if (!TerminateProcess(handle, 1)) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    public static string GetPath(ProcessSample sample)
    { using var handle = Open(sample, 0); var path = new StringBuilder(32768); int size = path.Capacity; if (!QueryFullProcessImageName(handle, 0, path, ref size)) throw new Win32Exception(Marshal.GetLastWin32Error()); return path.ToString(); }
    public static string Describe(ProcessSample sample)
    {
        using var handle = Open(sample, 0);
        var path = new StringBuilder(32768); int size = path.Capacity;
        string executable = QueryFullProcessImageName(handle, 0, path, ref size) ? path.ToString() : "Executable path unavailable";
        return $"{sample.Name}  ·  PID {sample.Id}  ·  Parent PID {sample.ParentId}\nExecutable: {executable}\nStarted: {DateTime.FromFileTimeUtc(sample.Created).ToLocalTime():G}\nWorking set : {sample.WorkingSet / 1048576d:N1} MiB  ·  Threads : {sample.Threads}  ·  Handles : {sample.Handles}\nSnapshot captured when the inspector opened. Protected processes may limit access.";
    }
    [DllImport("kernel32.dll", SetLastError = true)] private static extern SafeProcessHandle OpenProcess(uint access, bool inherit, int id);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetProcessTimes(SafeProcessHandle handle, out long created, out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool TerminateProcess(SafeProcessHandle handle, uint code);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool IsProcessCritical(SafeProcessHandle handle, [MarshalAs(UnmanagedType.Bool)] out bool critical);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool QueryFullProcessImageName(SafeProcessHandle handle, uint flags, StringBuilder path, ref int size);
}
