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
#define IDC_LOGIN_REFRESH   2004
#define IDC_CA              2005
#define IDC_CERT            2006
#define IDC_KEY             2007
#define IDC_BROWSE_CA       2008
#define IDC_BROWSE_CERT     2009
#define IDC_BROWSE_KEY      2010
#define IDC_POLL            2011
#define IDC_URL             2012
#define IDC_STREAM_URL      2013
#define IDC_SOFTWARE        2014
#define IDC_OVERLAY         2015
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
