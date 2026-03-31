add_library(CanaryReport STATIC
    SARIF.cpp
    BugReport.cpp
    BugReportMgr.cpp
    BugTypes.cpp
    ReportOptions.cpp
    SuppressionManager.cpp
)
target_link_libraries(CanaryReport CanaryDebugInfo CanaryFormats)
