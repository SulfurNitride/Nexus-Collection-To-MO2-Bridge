using Avalonia.Controls;
using NexusBridgeGui.ViewModels;

namespace NexusBridgeGui.Views;

public partial class Mo2SetupView : UserControl
{
    public Mo2SetupView()
    {
        InitializeComponent();
        AttachedToVisualTree += (s, e) =>
        {
            if (DataContext is Mo2SetupViewModel vm && TopLevel.GetTopLevel(this) is Window window)
            {
                vm.SetWindow(window);
            }
        };
    }
}
