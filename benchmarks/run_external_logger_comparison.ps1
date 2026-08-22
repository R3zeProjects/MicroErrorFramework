param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [int]$Operations = 500000,
    [int]$Samples = 7,
    [string]$Output = "external-logger-raw.csv"
)

$ErrorActionPreference = "Stop"
$scenarios = @("mef-sync", "spdlog-sync", "mef-async", "quill-async")
$producers = @(1, 4)
"sample,scenario,operations,producers,throughput_per_second,file_bytes" | Set-Content $Output

for ($sample = 1; $sample -le $Samples; ++$sample) {
    $ordered = if (($sample % 2) -eq 1) { $scenarios } else { $scenarios[($scenarios.Count - 1)..0] }
    foreach ($producerCount in $producers) {
        foreach ($scenario in $ordered) {
            $lines = & $Executable $scenario $Operations $producerCount
            if ($LASTEXITCODE -ne 0) { throw "$scenario failed" }
            $row = $lines | Select-Object -Last 1
            "$sample,$row" | Add-Content $Output
        }
    }
}

Write-Host "Raw alternating-order samples: $Output"
Import-Csv $Output |
    Group-Object scenario, producers |
    ForEach-Object {
        $values = @($_.Group.throughput_per_second | ForEach-Object { [double]$_ } | Sort-Object)
        [pscustomobject]@{
            Scenario = $_.Group[0].scenario
            Producers = $_.Group[0].producers
            MedianOpsPerSecond = [math]::Round($values[[int][math]::Floor($values.Count / 2)])
        }
    } |
    Sort-Object Scenario, Producers |
    Format-Table -AutoSize
