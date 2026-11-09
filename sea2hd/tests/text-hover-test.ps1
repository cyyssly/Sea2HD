# 大航海时代2 HD —— 文字悬停诊断脚本
#
# 用法（推荐）：双击本目录下的 运行文字悬停测试.cmd
# 或者在本目录打开 PowerShell 执行：
#   powershell -ExecutionPolicy Bypass -File .\text-hover-test.ps1
#
# 脚本会：清空诊断日志 -> 启动游戏 -> 每 5 秒留一张窗口截图（只保留最近 30 张）
#          -> 关闭游戏后把日志里的文字诊断行和截图收集到 tests\text-hover\

param(
    [int]$TimeoutMinutes = 20
)

$ErrorActionPreference = "Stop"

$gameDir = "d:\OldGames\大航海时代2"
$batPath = Join-Path $gameDir "大航海时代2(高清).bat"
$logPath = Join-Path $gameDir "hd\sea2-hd-diag.log"
$outDir  = Join-Path $gameDir "tests\text-hover"
$shotDir = Join-Path $outDir "shots"

function Pause-AtEnd {
    Write-Host ""
    Read-Host "按回车键关闭此窗口"
}

function Get-GameProcess {
    $all = @(Get-Process dosbox -ErrorAction SilentlyContinue)
    if ($all.Count -eq 0) { return $null }
    $withWindow = @($all | Where-Object { $_.MainWindowHandle -ne 0 })
    if ($withWindow.Count -gt 0) {
        $named = @($withWindow | Where-Object { $_.MainWindowTitle -like "*cycles*" })
        if ($named.Count -gt 0) { return $named[0] }
        return $withWindow[0]
    }
    return $all[0]
}

try {
    try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}

    New-Item -ItemType Directory -Force -Path $outDir, $shotDir | Out-Null
    Get-ChildItem $shotDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force

    Get-Process dosbox -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 800
    Remove-Item $logPath -ErrorAction SilentlyContinue

    Add-Type -AssemblyName System.Drawing
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Sea2Win {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  public struct RECT { public int Left, Top, Right, Bottom; }
  public struct POINT { public int X, Y; }
}
"@
    [Sea2Win]::SetProcessDPIAware() | Out-Null

    $startTime = Get-Date
    Write-Host "正在启动游戏，请稍候 ..." -ForegroundColor Cyan
    Start-Process -FilePath $batPath -WorkingDirectory $gameDir

    # 等游戏进程真正起来（.bat 里还要跑一段 PowerShell 算窗口尺寸，通常 1~3 秒）
    $waitUntil = (Get-Date).AddSeconds(90)
    $proc      = $null
    while ((Get-Date) -lt $waitUntil) {
        Start-Sleep -Milliseconds 500
        $proc = Get-GameProcess
        if ($proc) { break }
    }
    if (-not $proc) {
        Write-Host "90 秒内没有检测到 dosbox 进程，游戏可能启动失败。" -ForegroundColor Red
        Pause-AtEnd
        exit 1
    }
    Start-Sleep -Seconds 2

    Clear-Host
    Write-Host "===== 文字悬停诊断（已开始记录）=====" -ForegroundColor Cyan
    Write-Host "1. 等游戏进入画面后，把鼠标停在会变回繁体字的文字上，保持 3 秒以上。"
    Write-Host "2. 尽量在 2~3 个不同画面各做一次（主菜单、港内菜单、对话窗口等）。"
    Write-Host "3. 看到问题的那一刻按一次 Ctrl+F5（DOSBox 自带截图，最准确）。"
    Write-Host "4. 做完后直接关闭游戏窗口，脚本会自动收集结果。"
    Write-Host ""
    Write-Host "本窗口不要关闭，它正在后台截图并记录日志。" -ForegroundColor Yellow
    Write-Host ""

    $deadline  = (Get-Date).AddMinutes($TimeoutMinutes)
    $shotIndex = 0
    while ((Get-Date) -lt $deadline) {
        $proc = Get-GameProcess
        if (-not $proc) { break }

        if ($proc.MainWindowHandle -ne 0) {
            try {
                $r = New-Object Sea2Win+RECT
                [Sea2Win]::GetClientRect($proc.MainWindowHandle, [ref]$r) | Out-Null
                $pt = New-Object Sea2Win+POINT
                [Sea2Win]::ClientToScreen($proc.MainWindowHandle, [ref]$pt) | Out-Null
                $w  = $r.Right - $r.Left
                $hh = $r.Bottom - $r.Top
                if ($w -gt 100 -and $hh -gt 100) {
                    $bmp = New-Object System.Drawing.Bitmap $w, $hh
                    $g   = [System.Drawing.Graphics]::FromImage($bmp)
                    $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
                    $name = "shot_{0:D3}.png" -f $shotIndex
                    $bmp.Save((Join-Path $shotDir $name),
                              [System.Drawing.Imaging.ImageFormat]::Png)
                    $g.Dispose()
                    $bmp.Dispose()
                    ++$shotIndex
                    $all = Get-ChildItem $shotDir -Filter *.png | Sort-Object Name
                    if ($all.Count -gt 30) {
                        $all | Select-Object -First ($all.Count - 30) | Remove-Item -Force
                    }
                }
            } catch {
                # 窗口正在切换时截不到很正常，继续
            }
        }
        Start-Sleep -Seconds 5
    }

    if ((Get-Date) -ge $deadline) {
        Write-Host "已到 $TimeoutMinutes 分钟上限，停止记录。" -ForegroundColor Yellow
    }

    Write-Host "游戏已结束，正在收集结果 ..." -ForegroundColor Cyan

    if (Test-Path $logPath) {
        Copy-Item $logPath (Join-Path $outDir "sea2-hd-diag.log") -Force
        $patterns = '^(TXTFAIL|TXTCOLOR|TXTROW|TXTHOVER|  TXTCELL)'
        Select-String -Path $logPath -Pattern $patterns |
            ForEach-Object { $_.Line } |
            Set-Content (Join-Path $outDir "text-diag.txt") -Encoding UTF8
    }

    # DOSBox 的 Ctrl+F5 截图（默认落在 hd\capture 或 dosbox 配置目录）
    Get-ChildItem $gameDir -Recurse -Filter *.png -ErrorAction SilentlyContinue |
        Where-Object {
            $_.LastWriteTime -gt $startTime -and
            $_.FullName -notlike "*\assets\*" -and
            $_.FullName -notlike "*\tests\text-hover\*"
        } |
        ForEach-Object {
            Copy-Item $_.FullName (Join-Path $outDir ("dosbox_" + $_.Name)) -Force
        }

    $diag = Join-Path $outDir "text-diag.txt"
    if (Test-Path $diag) {
        $lines = (Get-Content $diag | Measure-Object -Line).Lines
        Write-Host "已收集诊断行：$lines 行 -> $diag" -ForegroundColor Green
    } else {
        Write-Host "没有收集到文字诊断行。" -ForegroundColor Yellow
    }
    Write-Host "截图目录：$shotDir" -ForegroundColor Green
    Write-Host ""
    Write-Host "请告诉我：你在哪个画面看到繁体字（主菜单 / 港内 / 对话 ...），以及“做完了”。" -ForegroundColor Cyan
} catch {
    Write-Host ""
    Write-Host "脚本出错：$($_.Exception.Message)" -ForegroundColor Red
    Write-Host $_.ScriptStackTrace -ForegroundColor DarkGray
}

Pause-AtEnd
