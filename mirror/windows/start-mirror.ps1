<#
  PC MIRROR SERVER (ffmpeg-based quick-tune version)
  ----------------------------------------------------
  This is the "afternoon of tuning" tier: same ffmpeg approach as
  before, but with encoder lookahead disabled (the single biggest free
  latency win) and an optional hardware-encode path. Expect roughly
  30-45ms glass-to-glass with -Encoder nvenc, vs 40-80ms before.

  For the real 15-20ms tier, use windows-native/ instead -- that
  replaces this whole script with a compiled app that skips ffmpeg
  entirely (native DXGI capture + Media Foundation hardware encode).

  USAGE:
    .\start-mirror.ps1 -Encoder nvenc      # NVIDIA GPU (recommended)
    .\start-mirror.ps1 -Encoder qsv        # Intel Quick Sync
    .\start-mirror.ps1 -Encoder cpu        # no GPU encoder available
#>

param(
    [int]$Port = 8080,
    [int]$Framerate = 60,
    [string]$Resolution = "1280x720",
    [ValidateSet("cpu","nvenc","qsv")]
    [string]$Encoder = "nvenc"
)

Write-Host "Starting PC mirror server on tcp://127.0.0.1:$Port (encoder: $Encoder) ..."
Write-Host "Make sure you've run:  adb reverse tcp:$Port tcp:$Port"
Write-Host ""

$gop = $Framerate * 2

switch ($Encoder) {
    "cpu" {
        ffmpeg `
          -f gdigrab -framerate $Framerate -i desktop `
          -vf "scale=$Resolution" `
          -pix_fmt yuv420p `
          -c:v libx264 `
          -preset ultrafast `
          -tune zerolatency `
          -profile:v baseline `
          -x264-params "repeat-headers=1:keyint=$gop:min-keyint=$Framerate:rc-lookahead=0:sync-lookahead=0" `
          -bf 0 `
          -f h264 `
          "tcp://127.0.0.1:$Port`?listen=1"
    }
    "nvenc" {
        # -delay 0 and -zerolatency 1 disable NVENC's internal frame
        # buffering; dump_extra re-inserts SPS/PPS before every keyframe
        # since nvenc doesn't have an x264-style repeat-headers flag.
        ffmpeg `
          -f gdigrab -framerate $Framerate -i desktop `
          -vf "scale=$Resolution" `
          -pix_fmt yuv420p `
          -c:v h264_nvenc `
          -preset p1 `
          -tune ull `
          -rc cbr `
          -zerolatency 1 `
          -delay 0 `
          -g $gop `
          -bf 0 `
          -profile:v baseline `
          -bsf:v dump_extra=freq=keyframe `
          -f h264 `
          "tcp://127.0.0.1:$Port`?listen=1"
    }
    "qsv" {
        ffmpeg `
          -f gdigrab -framerate $Framerate -i desktop `
          -vf "scale=$Resolution" `
          -pix_fmt yuv420p `
          -c:v h264_qsv `
          -preset veryfast `
          -low_power 1 `
          -g $gop `
          -bf 0 `
          -profile:v baseline `
          -bsf:v dump_extra=freq=keyframe `
          -f h264 `
          "tcp://127.0.0.1:$Port`?listen=1"
    }
}
