using System.Diagnostics;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
namespace CyWinTask;
public sealed class ProcessDetailsWindow : Window
{
    public ProcessDetailsWindow(ProcessSample sample)
    {
        Title = $"Advanced details — {sample.Name} ({sample.Id})";
        Width=850; Height=650; MinWidth=580; MinHeight=400;
        Background=UiTheme.Brush("Bg1"); Foreground=UiTheme.Brush("Text");
        WindowStartupLocation=WindowStartupLocation.CenterOwner;
        var dock=new DockPanel { Margin=new Thickness(22) };
        var header=new DockPanel { Margin=new Thickness(0,0,0,15) };
        var text=new TextBox { Text="Loading details…",IsReadOnly=true,AcceptsReturn=true,TextWrapping=TextWrapping.Wrap,
            VerticalScrollBarVisibility=ScrollBarVisibility.Auto,FontFamily=new FontFamily("Consolas"),FontSize=13,
            Background=UiTheme.Brush("Bg2"),BorderBrush=UiTheme.Brush("LineStrong"),BorderThickness=new Thickness(1),
            Padding=new Thickness(14) };
        var close=new Button { Content="Fermer" };
        close.Click+=(_,_)=>Close();
        DockPanel.SetDock(close,Dock.Right); header.Children.Add(close);
        var copy=new Button { Content="Copy all", ToolTip="Copy all displayed details" };
        copy.Click+=(_,_)=> { try { Clipboard.SetText(text.Text); } catch (Exception ex) { MessageBox.Show(this, ex.Message, "Copy unavailable", MessageBoxButton.OK, MessageBoxImage.Information); } };
        DockPanel.SetDock(copy,Dock.Right); header.Children.Add(copy);
        header.Children.Add(new TextBlock { Text=$"{sample.Name} · PID {sample.Id}",FontSize=23,FontWeight=FontWeights.SemiBold,VerticalAlignment=VerticalAlignment.Center });
        DockPanel.SetDock(header,Dock.Top); dock.Children.Add(header);
        var footer=new TextBlock { Text="Snapshot taken when opened · protected processes may restrict access",Margin=new Thickness(0,12,0,0),TextWrapping=TextWrapping.Wrap,Foreground=UiTheme.Brush("TextDim") };
        DockPanel.SetDock(footer,Dock.Bottom); dock.Children.Add(footer);
        dock.Children.Add(text); Content=dock;
        Loaded+=async(_,_)=> { var report=await Task.Run(()=>ReadReport(sample)); if(IsVisible)text.Text=report; };
    }
    public static string ReadReport(ProcessSample sample)
    {
        var b=new StringBuilder();
        b.AppendLine($"Process: {sample.Name}\nPID: {sample.Id}\nParent PID: {sample.ParentId}\nCaptured: {DateTime.Now:G}\n");
        void Add(string label,Func<object?> read)
        { try{b.AppendLine(label+" : "+read());}catch(Exception ex){b.AppendLine(label+": unavailable ("+ex.Message+")");} }
        string? path=null;
        Add("Executable",()=>path=ProcessActions.GetPath(sample));
        if(path!=null)
        {
            Add("Description",()=>FileVersionInfo.GetVersionInfo(path).FileDescription);
            Add("Reported publisher",()=>FileVersionInfo.GetVersionInfo(path).CompanyName);
            Add("File version",()=>FileVersionInfo.GetVersionInfo(path).FileVersion);
        }
        b.AppendLine("\nPROCESS COUNTERS");
        try
        {
            using var process=Process.GetProcessById(sample.Id);
            _=process.Handle; // Retain one process handle before reading identity and properties.
            if(process.StartTime.ToUniversalTime().ToFileTimeUtc()!=sample.Created)throw new InvalidOperationException("The process exited or its PID was reused.");
            Add("Start time",()=>process.StartTime);
            Add("Session",()=>process.SessionId);
            Add("Priority",()=>process.PriorityClass);
            Add("CPU affinity (hex mask)",()=>"0x"+process.ProcessorAffinity.ToInt64().ToString("X"));
            Add("Total CPU time",()=>process.TotalProcessorTime);
            Add("User CPU time",()=>process.UserProcessorTime);
            Add("Kernel CPU time",()=>process.PrivilegedProcessorTime);
            Add("Working set",()=>$"{process.WorkingSet64/1048576d:N1} MiB");
            Add("Private memory",()=>$"{process.PrivateMemorySize64/1048576d:N1} MiB");
            Add("Virtual address space",()=>$"{process.VirtualMemorySize64/1048576d:N1} MiB");
            Add("Peak working set",()=>$"{process.PeakWorkingSet64/1048576d:N1} MiB");
            Add("Threads",()=>process.Threads.Count);
            Add("Handles",()=>process.HandleCount);
            b.AppendLine("\nLOADED MODULES (up to 200)");
            try
            {
                var modules=process.Modules;
                foreach(ProcessModule module in modules.Cast<ProcessModule>().Take(200))
                    b.AppendLine($"{module.ModuleName} · {module.ModuleMemorySize/1024:N0} KiB\n  {module.FileName}");
                if(modules.Count>200)b.AppendLine($"… {modules.Count-200} additional modules omitted.");
            }
            catch(Exception ex){b.AppendLine("Modules unavailable: "+ex.Message);}
        }
        catch(Exception ex){b.AppendLine("Details unavailable: "+ex.Message);}
        return b.ToString();
    }
}
