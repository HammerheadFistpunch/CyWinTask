using System.Runtime.InteropServices;
namespace CyWinTask;
public static class CommitMemory
{
    public static PerfMetric Read()
    {
        var info = new Information();
        if (!GetPerformanceInfo(out info, (uint)Marshal.SizeOf<Information>())) return new("commit", "Committed memory", null, "GiB");
        return new("commit", "Committed memory", info.Total * (double)info.PageSize / 1073741824, "GiB", info.Limit * (double)info.PageSize / 1073741824);
    }
    [StructLayout(LayoutKind.Sequential)] private struct Information
    {
        public uint Size;
        public ulong Total, Limit, Peak, Physical, Available, Cache, Kernel, Paged, Nonpaged, PageSize;
        public uint Handles, Processes, Threads;
    }
    [DllImport("psapi.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetPerformanceInfo(out Information info, uint size);
}
