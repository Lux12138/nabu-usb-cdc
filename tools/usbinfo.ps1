$filter = 'VID_2717|VID_18D1|VID_05C6|VID_1209'
$devs = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match $filter }
foreach ($x in $devs) {
  $loc = (Get-PnpDeviceProperty -InstanceId $x.InstanceId -KeyName 'DEVPKEY_Device_LocationInfo' -ErrorAction SilentlyContinue).Data
  $par = (Get-PnpDeviceProperty -InstanceId $x.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
  "$($x.Status) | $($x.Class) | $($x.FriendlyName) | $($x.InstanceId) | loc=$loc | parent=$par"
  if ($par) {
    $p = Get-PnpDevice -InstanceId $par -ErrorAction SilentlyContinue
    if ($p) { "    parent: $($p.FriendlyName) [$($p.InstanceId)]" }
    $gp = (Get-PnpDeviceProperty -InstanceId $par -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue).Data
    if ($gp) {
      $g = Get-PnpDevice -InstanceId $gp -ErrorAction SilentlyContinue
      if ($g) { "    grandparent: $($g.FriendlyName) [$($g.InstanceId)]" }
    }
  }
}
