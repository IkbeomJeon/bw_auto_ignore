$path = 'C:\wd\bw_auto_ignore\bw_auto_ignore\bw_auto_ignore.rc'
$lines = [System.IO.File]::ReadAllLines($path, [System.Text.Encoding]::Unicode)

$newLines = [System.Collections.Generic.List[string]]::new()
foreach ($line in $lines) {
    # 잘못 붙은 'nCONTROL' 를 두 줄로 분리
    if ($line -match '^(CONTROL.*IDC_AUTO_SHOW_STATS.*10, 38, 190, 10)nCONTROL(.*)$') {
        $newLines.Add($Matches[1])
        $newLines.Add('CONTROL' + $Matches[2])
    } else {
        $newLines.Add($line)
    }
}

[System.IO.File]::WriteAllLines($path, $newLines, [System.Text.Encoding]::Unicode)
Write-Host "Done: $($newLines.Count) lines"
