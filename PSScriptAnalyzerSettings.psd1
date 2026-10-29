# PSScriptAnalyzer's defaults, minus rules that fight how these build and test scripts are written.
@{
    ExcludeRules = @(
        'PSAvoidUsingWriteHost'                        # progress for the person running the script
        'PSAvoidUsingPositionalParameters'
        'PSUseShouldProcessForStateChangingFunctions'  # internal helpers, never run with -WhatIf
    )
}
