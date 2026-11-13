using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace CyWinTask;
public sealed class MetricHistory
{
    public PerfMetric Metric { get; set; }
    public List<(DateTime Time, double? Value)> Points { get; } = new(64);
    public MetricHistory(PerfMetric metric) => Metric = metric;
    public void Add(DateTime time, PerfMetric metric)
    {
        Metric = metric;
        Points.RemoveAll(p => p.Time < time.AddSeconds(-60));
        if (Points.Count >= 120) Points.RemoveAt(0);
        Points.Add((time, metric.Value));
    }
}
public sealed class PerformanceResource : INotifyPropertyChanged
{
    public PerfDevice Device { get; private set; }
    public string Title => Device.Title;
    public string Subtitle => Device.Description;
    public string Summary
    {
        get
        {
            string usage = Device.Metrics[0].Value is double value ? $"{value:N1} {Device.Metrics[0].Unit}" : "Unavailable / waiting for samples";
            var temperature = Device.Metrics.FirstOrDefault(m => m.Id == "temperature")?.Value;
            return temperature is double degrees ? $"{usage} · {degrees:N0} °C" : usage;
        }
    }
    public Dictionary<string, MetricHistory> Histories { get; } = new();
    public PerformanceResource(PerfDevice device) => Device = device;
    public void Update(DateTime time, PerfDevice device)
    {
        Device = device;
        foreach (var metric in device.Metrics)
        {
            if (!Histories.TryGetValue(metric.Id, out var history)) Histories[metric.Id] = history = new(metric);
            history.Add(time, metric);
        }
        PropertyChanged?.Invoke(this, new(nameof(Summary)));
        PropertyChanged?.Invoke(this, new(nameof(Subtitle)));
    }
    public event PropertyChangedEventHandler? PropertyChanged;
}

public partial class PerformanceView : UserControl
{
    public ObservableCollection<PerformanceResource> ResourcesList { get; } = new();
    public event EventHandler? PauseRequested;
    private readonly Dictionary<string, PerformanceResource> resources = new();
    private readonly Dictionary<string, (Window Window, GraphDashboard Dashboard, Button Pause)> detached = new();
    private bool graphMode;
    public bool IsGraphMode => graphMode;
    public bool HasDetachedWindow => detached.Count > 0;
    private readonly List<(HistoryChart Chart, TextBlock Value)> charts = new();
    private string layoutKey = ""; private readonly List<(MetricHistory History, TextBlock Label)> legend = new();
    public PerformanceView() { InitializeComponent(); Devices.ItemsSource = ResourcesList; Dashboard.OpenResourceRequested += id => OpenDashboard(id); }
    public void SetPaused(bool paused) { Pause.Content = paused ? "▶  Resume" : "Ⅱ  Pause"; foreach (var item in detached.Values) item.Pause.Content = Pause.Content; if (paused) CollectionStatus.Text = "Updates paused"; }
    public void ShowError(string message) => CollectionStatus.Text = message;
    public void Apply(PerfSnapshot snapshot)
    {
        var present = snapshot.Devices.Select(d => d.Id).ToHashSet();
        foreach (var id in resources.Keys.Where(id => !present.Contains(id)).ToArray())
        { ResourcesList.Remove(resources[id]); resources.Remove(id); }
        foreach (var device in snapshot.Devices)
        {
            if (!resources.TryGetValue(device.Id, out var resource))
            { resources[device.Id] = resource = new(device); ResourcesList.Add(resource); }
            resource.Update(snapshot.Time, device);
        }
        if (Devices.SelectedItem == null && ResourcesList.Count > 0) Devices.SelectedIndex = 0;
        if (IsVisible && !graphMode) RefreshCharts();
        if (IsVisible && graphMode) Dashboard.Update(ResourcesList);
        foreach (var item in detached.Values) item.Dashboard.Update(ResourcesList);
        CollectionStatus.Text = $"{snapshot.Time.ToLocalTime():HH:mm:ss} · Windows counters {snapshot.CollectionMs:N1} ms · 60 s history · Sampling while Performance or a detached window is open";
    }
    private void Pause_Click(object sender, RoutedEventArgs e) => PauseRequested?.Invoke(this, EventArgs.Empty);
    public void ShowGraphMode(bool show)
    {
        graphMode = show;
        DeviceLayout.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        Dashboard.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        GraphMode.Content = show ? "Detailed view" : "Graph view";
        RefreshVisible();
    }
    public void RefreshVisible()
    {
        if (graphMode) Dashboard.Update(ResourcesList);
        else RefreshCharts();
    }
    private void GraphMode_Click(object sender, RoutedEventArgs e) => ShowGraphMode(!graphMode);
    private void OpenWindow_Click(object sender, RoutedEventArgs e) => OpenDashboard();
    public void OpenDashboard(string? resourceId = null)
    {
        string key = resourceId ?? "all";
        if (detached.TryGetValue(key, out var existing))
        {
            if (existing.Window.WindowState == WindowState.Minimized) existing.Window.WindowState = WindowState.Normal;
            existing.Window.Activate();
            return;
        }
        var board = new GraphDashboard { FilterId = resourceId, AllowPopOut = false };
        var window = new Window
        {
            Title = resourceId == null ? "CyWinTask · CPU, RAM, and GPU graphs" : "CyWinTask · Graphique",
            Icon = new BitmapImage(new Uri("pack://application:,,,/CyWinTask;component/Assets/cywintask-logo-transparent.png")),
            Width = resourceId == null ? 900 : 760,
            Height = resourceId == null ? 740 : 390,
            MinWidth = 430,
            MinHeight = 300,
            Background = UiTheme.Brush("Bg1"),
            Foreground = UiTheme.Brush("Text"),
            WindowStartupLocation = WindowStartupLocation.CenterScreen
        };
        var root = new DockPanel { Margin = new Thickness(18) };
        var header = new DockPanel { Margin = new Thickness(0, 0, 0, 12) };
        var pause = new Button { Content = Pause.Content, Padding = new Thickness(12, 6, 12, 6) };
        pause.Click += (_, _) => PauseRequested?.Invoke(this, EventArgs.Empty);
        DockPanel.SetDock(pause, Dock.Right);
        header.Children.Add(pause);
        header.Children.Add(new TextBlock
        {
            Text = resourceId == null ? "CPU · RAM · GPU" : "Performance graph",
            FontSize = 23,
            FontWeight = FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center
        });
        DockPanel.SetDock(header, Dock.Top);
        root.Children.Add(header);
        root.Children.Add(board);
        window.Content = root;
        window.Closed += (_, _) => detached.Remove(key);
        detached[key] = (window, board, pause);
        board.Update(ResourcesList);
        window.Show();
    }
    public void CloseDetachedWindows()
    {
        foreach (var item in detached.Values.ToArray()) item.Window.Close();
    }
    private void Device_Changed(object sender, SelectionChangedEventArgs e) => RefreshCharts();
    private void Cores_Changed(object sender, RoutedEventArgs e) => RefreshCharts();
    private void CpuMode_Changed(object sender, SelectionChangedEventArgs e) { if (Charts != null) RefreshCharts(); }
    private void RefreshCharts()
    {
        if (Devices.SelectedItem is not PerformanceResource resource) return;
        var device = resource.Device;
        DeviceTitle.Text = device.Title;
        DeviceDescription.Text = device.Description + (device.Kind == "gpu" ? " · " + resource.Summary : "");
        ShowCores.Visibility = device.Kind is "cpu" or "gpu" ? Visibility.Visible : Visibility.Collapsed; ShowCores.Content = device.Kind == "gpu" ? "Show other GPU engines" : "Show logical processors";
        var mainEngines = new[] { "engine:3D", "engine:Copy", "engine:VideoEncode", "engine:VideoDecode", "dedicated", "shared" };
        var visible = device.Metrics.Where(m => m.Id != "temperature" && (device.Kind != "cpu" || !m.Id.StartsWith("core:") || ShowCores.IsChecked == true) && (device.Kind != "gpu" || (m.Id != "usage" && (mainEngines.Contains(m.Id) || ShowCores.IsChecked == true)))).ToArray();
        bool compact = device.Kind == "cpu" && ShowCores.IsChecked == true && CpuMode.SelectedIndex == 0;
        CpuModePanel.Visibility = device.Kind == "cpu" ? Visibility.Visible : Visibility.Collapsed;
        if (compact && visible.Length > 1) visible = visible.Where(m => m.Id.StartsWith("core:")).ToArray();
        if (device.Kind == "cpu") DeviceDescription.Text = device.Description + " · " + resource.Summary;
        string key = compact + ":" + device.Id + ":" + string.Join(',', visible.Select(m => m.Id));
        if (key != layoutKey)
        {
            layoutKey = key; charts.Clear(); Charts.Children.Clear(); legend.Clear(); CombinedLegend.Children.Clear();
            Charts.Columns = compact ? Math.Clamp((int)Math.Ceiling(Math.Sqrt(visible.Length * 1.5)), 2, 8) : device.Kind == "gpu" || visible.Length > 3 ? 2 : 1;
            Brush accent = device.Kind switch { "memory" => UiTheme.Brush("Memory"), "disk" => UiTheme.Brush("Disk"), "network" => UiTheme.Brush("Network"), _ => UiTheme.Brush("Info") };
            if (device.Kind == "disk")
            {
                var colors = new Brush[] { UiTheme.Brush("Disk"), UiTheme.Brush("Info"), UiTheme.Brush("Network") };
                var combined = new HistoryChart(resource.Histories[visible[0].Id]) { Accent = colors[0], Height = 330 };
                for (int i = 0; i < visible.Length; i++)
                {
                    var history = resource.Histories[visible[i].Id];
                    if (i > 0) combined.AdditionalSeries.Add((history, colors[i]));
                    var label = new TextBlock { Foreground = colors[i], Margin = new Thickness(0,0,24,8) }; CombinedLegend.Children.Add(label); legend.Add((history,label));
                }
                Charts.Children.Add(combined); charts.Add((combined,new TextBlock()));
            }
            else foreach (var metric in visible)
            {
                if (!resource.Histories.TryGetValue(metric.Id, out var history)) continue;
                var container = new StackPanel { Margin = new Thickness(0, 0, 12, 18) };
                var header = new DockPanel { Margin = new Thickness(0, 0, 0, 7) };
                var value = new TextBlock { HorizontalAlignment = HorizontalAlignment.Right, Foreground = accent, FontSize = compact ? 10 : 13 };
                DockPanel.SetDock(value, Dock.Right); header.Children.Add(value);
                header.Children.Add(new TextBlock { Text = compact ? metric.Label.Replace("Logical processor ", "CPU ") : metric.Label, FontSize = compact ? 10 : 13, TextTrimming = TextTrimming.CharacterEllipsis, Margin = new Thickness(0, 0, 10, 0) });
                var chart = new HistoryChart(history) { Accent = accent, Height = compact ? Math.Clamp(340d / Math.Ceiling(visible.Length / (double)Charts.Columns) - 27, 32, 80) : device.Kind == "gpu" ? 100 : device.Kind == "memory" ? 125 : visible.Length > 3 ? 95 : 165 };
                container.Children.Add(header); container.Children.Add(chart);
                Charts.Children.Add(container); charts.Add((chart, value));
            }
        }
        foreach (var (chart, value) in charts)
        {
            var metric = chart.History.Metric;
            value.Text = metric.Value is double number ? $"{number:N1} {metric.Unit}" : "Unavailable";
            chart.InvalidateVisual();
        }
        foreach (var (history,label) in legend) label.Text = $"● {history.Metric.Label} : " + (history.Metric.Value is double v ? $"{v:N1} {history.Metric.Unit}" : "Unavailable");
        DeviceNote.Text = device.Kind switch
        {
            "gpu" => "Usage reflects the busiest GPU engine. Engine and temperature readings depend on the driver and sensors.",
            "network" => "Live throughput in Mbps with automatic scaling. Includes virtual interfaces.",
            "disk" => "Activity (green), read (blue), and write (orange). The left axis shows 0–100%. The right axis scales throughput in MiB/s.",
            "cpu" => "CPU usage comes from Windows counters. CPU temperature requires LibreHardwareMonitor with WMI enabled. Usage can differ from Task Manager.",
            _ => "Committed memory is backed by RAM or page files. The ceiling shows the current commit limit."
        };
    }
}
