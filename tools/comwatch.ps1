param([int]$Seconds = 240, [string]$Log = 'E:\nabu\logs\comwatch.log')
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
  $t = Get-Date -Format 'HH:mm:ss'
  $ports = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue |
           ForEach-Object { $_.FriendlyName + '|' + $_.InstanceId }
  $com = [System.IO.Ports.SerialPort]::GetPortNames() -join ','
  "$t com=$com ports=$($ports -join '; ')" | Out-File -Append -Encoding utf8 $Log
  Start-Sleep -Seconds 2
}
