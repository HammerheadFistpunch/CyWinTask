using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace CyWinTask;

// Draws the same bounded histories as the detailed Performance view.
public sealed class GraphDashboard : UserControl
{
    private readonly StackPanel cards = new() { Margin = new Thickness(0, 2, 8, 8) };
    private readonly List<(HistoryChart Chart, TextBlock Value, TextBlock? Temperature, PerformanceResource Resource)> visible = new();
    private string layoutKey = "";
    public string? FilterId { get; set; }
    public bool AllowPopOut { get; set; } = true;
    public event Action<string>? OpenResourceRequested;

    public GraphDashboard()
    {
        Content = new ScrollViewer
        {
            Content = cards,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled
        };
    }

    public void Update(IEnumerable<PerformanceResource> resources)
    {
        var chosen = resources.Where(r => (r.Device.Kind is "cpu" or "memory" or "gpu")
            && (FilterId == null || r.Device.Id == FilterId))
            .Select(r => (Resource: r, MetricId: r.Device.Kind switch
            {
                "cpu" => "cpu", "memory" => "used", _ => "usage"
            }))
            .Where(x => x.Resource.Histories.ContainsKey(x.MetricId))
            .ToArray();
        string key = string.Join('|', chosen.Select(x => x.Resource.Device.Id + "/" + x.MetricId));
        if (key != layoutKey)
        {
            layoutKey = key;
            cards.Children.Clear();
            visible.Clear();
            foreach (var (resource, metricId) in chosen)
            {
                var device = resource.Device;
                var color = device.Kind switch
                {
                    "memory" => UiTheme.Brush("Memory"),
                    "gpu" => UiTheme.Brush("Disk"),
                    _ => UiTheme.Brush("Info")
                };
                var card = new Border
                {
                    Background = UiTheme.Brush("Bg2"),
                    BorderBrush = UiTheme.Brush("LineStrong"),
                    BorderThickness = new Thickness(1),
                    CornerRadius = new CornerRadius(7),
                    Padding = new Thickness(16, 12, 16, 14),
                    Margin = new Thickness(0, 0, 0, 12)
                };
                var body = new StackPanel();
                var titleRow = new DockPanel();
                if (AllowPopOut)
                {
                    var open = new Button { Content = "Open this graph", Padding = new Thickness(9, 5, 9, 5) };
                    open.Click += (_, _) => OpenResourceRequested?.Invoke(device.Id);
                    DockPanel.SetDock(open, Dock.Right);
                    titleRow.Children.Add(open);
                }
                var title = new TextBlock
                {
                    Text = device.Kind switch
                    {
                        "cpu" => "Processor", "memory" => "Physical memory", _ => device.Title
                    },
                    FontSize = 19,
                    FontWeight = FontWeights.SemiBold,
                    VerticalAlignment = VerticalAlignment.Center
                };
                titleRow.Children.Add(title);
                body.Children.Add(titleRow);
                if (device.Kind == "gpu") body.Children.Add(new TextBlock
                {
                    Text = device.Description,
                    Foreground = UiTheme.Brush("TextDim"),
                    TextTrimming = TextTrimming.CharacterEllipsis,
                    Margin = new Thickness(0, 4, 0, 4)
                });
                var value = new TextBlock { Foreground = color, Margin = new Thickness(0, 5, 0, 8) };
                body.Children.Add(value);
                TextBlock? temperature = null;
                if (device.Kind is "cpu" or "gpu")
                {
                    temperature = new TextBlock { Foreground = UiTheme.Brush("TextDim"), Margin = new Thickness(0, 0, 0, 8) };
                    body.Children.Add(temperature);
                }
                var chart = new HistoryChart(resource.Histories[metricId]) { Accent = color, Height = 145 };
                body.Children.Add(chart);
                card.Child = body;
                cards.Children.Add(card);
                visible.Add((chart, value, temperature, resource));
            }
            if (chosen.Length == 0) cards.Children.Add(new TextBlock
            {
                Text = "Waiting for CPU, memory, and GPU readings…",
                Foreground = UiTheme.Brush("TextDim"),
                Margin = new Thickness(12)
            });
        }
        foreach (var (chart, value, temperature, resource) in visible)
        {
            var metric = chart.History.Metric;
            value.Text = metric.Value is double number
                ? resource.Device.Kind == "memory" && metric.Maximum is double maximum && maximum > 0
                    ? $"{number:N1} / {maximum:N1} GiB · {number / maximum * 100:N0} %"
                    : $"{number:N1} {metric.Unit}"
                : "Unavailable or waiting for samples";
            if (temperature != null)
            {
                var sensor = resource.Device.Metrics.FirstOrDefault(m => m.Id == "temperature");
                temperature.Text = sensor?.Value is double degrees ? $"Temperature: {degrees:N0} °C" : "Temperature: indisponible";
                temperature.ToolTip = resource.Device.Kind == "cpu" ? "CPU sensor provided by LibreHardwareMonitor when available" : "Sensor provided by the GPU driver";
            }
            chart.InvalidateVisual();
        }
    }
}
