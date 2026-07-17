#pragma once

// Control IDs for the settings dialog (resources/gig.rc IDD_SETTINGS).
// Shared by the resource compiler and src/ui/settings_dialog.cpp.

#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif

#define IDD_SETTINGS        2000
#define IDC_BASE            2001
#define IDC_USER            2002
#define IDC_PASSWORD        2003
#define IDC_INSECURE        2016
#define IDC_STATUS          2017
#define IDD_SETTINGS_ADVANCED 2018
#define IDC_ADVANCED        2019
#define IDC_LABELMODE       2020
// TODO(onboarding-project): temporary Forget Settings affordance; remove when done.
#define IDC_FORGET          2021
#define IDC_DIM_LEVEL       2022
#define IDC_DIM_DELAY       2023
#define IDC_DIM_LEVEL_VAL   2024
#define IDC_ORBIT_STEP      2025
#define IDC_VIEW_MODE       2026
#define IDC_MOTION_ACTIVITY 2027
#define IDC_STREAM_HIDDEN   2028
#define IDC_ACTIVE_ONLY     2029
#define IDC_SHOW_BOXES      2030
#define IDC_LABELSIZE       2031
