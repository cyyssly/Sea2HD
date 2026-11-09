@echo off
setlocal
cd /d "%~dp0hd"
rem 窗口顶端贴屏幕顶端、底端贴任务栏；宽度按 640x480 的 4:3 计算
rem window_size 与 window_position 都是逻辑像素（受系统 DPI 缩放）；PowerShell 默认非 DPI 感知，取到的度量正好是同一套逻辑坐标
rem 顶部偏移 = 标题栏(SM_CYCAPTION) + 边框(SM_CYFRAME) + 内边距(SM_CXPADDEDBORDER，逻辑坐标下恒为 4)
rem 底部不要减边框：窗口外框最下面有一段不可见的调整边框，减掉它就会在客户区与任务栏之间露出一条桌面缝隙
set "SEA2_WINSIZE=1280x960"
set "SEA2_WINPOS=auto"
for /f "usebackq tokens=1,2" %%a in (`powershell -NoProfile -Command "Add-Type -AssemblyName System.Windows.Forms; $s=[System.Windows.Forms.Screen]::PrimaryScreen; $si=[System.Windows.Forms.SystemInformation]; $t=$si::CaptionHeight+$si::FrameBorderSize.Height+4; $y=$s.WorkingArea.Top+$t-1; $h=$s.WorkingArea.Bottom-$y+1; if($h -lt 480){$h=480}; $w=[int]($h*4/3); $x=[int](($s.Bounds.Width-$w)/2); ($w.ToString()+'x'+$h.ToString()+' '+$x.ToString()+','+$y.ToString())"`) do (
    set "SEA2_WINSIZE=%%a"
    set "SEA2_WINPOS=%%b"
)
start "" "%~dp0hd\dosbox-staging\dosbox.exe" --conf "%~dp0hd\sea2-hd.conf" --nolocalconf --set "window_size=%SEA2_WINSIZE%" --set "window_position=%SEA2_WINPOS%"