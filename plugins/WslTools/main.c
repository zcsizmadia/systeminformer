/*
 * Copyright (c) 2022 Winsider Seminars & Solutions, Inc.  All rights reserved.
 *
 * This file is part of System Informer.
 *
 * Authors:
 *
 *     zcsizmadia   2026
 *
 */

#include "wsltools.h"

PPH_PLUGIN PluginInstance = NULL;
static PH_CALLBACK_REGISTRATION PluginLoadCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginUnloadCallbackRegistration;
static PH_CALLBACK_REGISTRATION MainWindowShowingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;
static PH_CALLBACK_REGISTRATION SystemInformationInitializingCallbackRegistration;
static PH_CALLBACK_REGISTRATION ProcessMenuInitializingCallbackRegistration;
static PH_CALLBACK_REGISTRATION PluginMenuItemCallbackRegistration;
static BOOLEAN WslInstalled = FALSE;

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI LoadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    // Checked once at startup: without WSL there is no tab and no provider thread.
    WslInstalled = WslIsInstalled();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI UnloadCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    BOOLEAN sessionEnding = (BOOLEAN)PtrToUlong(Parameter);

    // At logoff the process is about to end anyway, so do not wait for a command in progress.
    WslStopProvider(!sessionEnding);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MainWindowShowingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (!WslInstalled)
        return;

    WslInitializeSnapshotType();
    WslInitializeProcessFrameType();
    WslInitializeTab();
    WslStartProvider();
}

static VOID NTAPI ProcessesUpdatedInvoke(
    _In_opt_ PVOID Parameter
    )
{
    WslOnProcessesUpdated();
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessesUpdatedCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    // Raised on the process provider thread; the tab is updated on the GUI thread.
    if (WslInstalled)
        SystemInformer_Invoke(ProcessesUpdatedInvoke, NULL);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI SystemInformationInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    if (WslInstalled && Parameter)
        WslSystemInformationInitializing(Parameter);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI ProcessMenuInitializingCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_INFORMATION menuInfo = Parameter;
    PPH_PROCESS_ITEM processItem;
    PPH_PROCESS_ITEM wslVmItem;
    PPH_PROCESS_ITEM sessionVmItem;
    WSL_VM_SELECTION selection = WslVmSelectionNone;
    PPH_EMENU_ITEM propertiesItem;
    ULONG index;

    if (!WslInstalled || !menuInfo || menuInfo->u.Process.NumberOfProcesses != 1)
        return;

    processItem = menuInfo->u.Process.Processes[0];

    // This runs on every process menu, so rule out other processes before enumerating.
    if (!processItem->ProcessName || !WslIsVmProcessName(processItem->ProcessName))
        return;

    // Only a vmmem the tab can show gets the item, using the same matching as the tab.
    wslVmItem = WslReferenceVmProcessItem(NULL);
    sessionVmItem = WslReferenceSessionVmProcessItem();

    if (wslVmItem && wslVmItem->ProcessId == processItem->ProcessId)
        selection = WslVmSelectionWsl;
    else if (sessionVmItem && sessionVmItem->ProcessId == processItem->ProcessId)
        selection = WslVmSelectionSession;

    PhClearReference(&wslVmItem);
    PhClearReference(&sessionVmItem);

    if (selection == WslVmSelectionNone)
        return;

    // Place it just above Properties, or last if the menu has none.
    index = ULONG_MAX;

    if (propertiesItem = PhFindEMenuItem(menuInfo->Menu, 0, NULL, PHAPP_ID_PROCESS_PROPERTIES))
        index = PhIndexOfEMenuItem(menuInfo->Menu, propertiesItem);

    PhInsertEMenuItem(menuInfo->Menu, PhPluginCreateEMenuItem(PluginInstance, 0, ID_PROCESS_GOTOWSL, L"&Go to WSL", UlongToPtr(selection)), index);
}

_Function_class_(PH_CALLBACK_FUNCTION)
static VOID NTAPI MenuItemCallback(
    _In_opt_ PVOID Parameter,
    _In_opt_ PVOID Context
    )
{
    PPH_PLUGIN_MENU_ITEM menuItem = Parameter;

    if (menuItem && menuItem->Id == ID_PROCESS_GOTOWSL)
        WslSelectVmNode((WSL_VM_SELECTION)PtrToUlong(menuItem->Context));
}

LOGICAL DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _Reserved_ PVOID Reserved
    )
{
    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        {
            PPH_PLUGIN_INFORMATION info;
            PH_SETTING_CREATE settings[] =
            {
                { StringSettingType, SETTING_NAME_TREE_LIST_COLUMNS, L"" },
                { IntegerPairSettingType, SETTING_NAME_TREE_LIST_SORT, L"0,1" }, // Name, ascending
            };

            PluginInstance = PhRegisterPlugin(PLUGIN_NAME, Instance, &info);

            if (!PluginInstance)
                return FALSE;

            info->DisplayName = L"WSL Tools";
            info->Description = L"Shows WSL distributions and the WSL 2 virtual machine in a WSL tab and in System Information.";

            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackLoad),
                LoadCallback,
                NULL,
                &PluginLoadCallbackRegistration
                );
            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackUnload),
                UnloadCallback,
                NULL,
                &PluginUnloadCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackMainWindowShowing),
                MainWindowShowingCallback,
                NULL,
                &MainWindowShowingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessesUpdated),
                ProcessesUpdatedCallback,
                NULL,
                &ProcessesUpdatedCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackSystemInformationInitializing),
                SystemInformationInitializingCallback,
                NULL,
                &SystemInformationInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetGeneralCallback(GeneralCallbackProcessMenuInitializing),
                ProcessMenuInitializingCallback,
                NULL,
                &ProcessMenuInitializingCallbackRegistration
                );
            PhRegisterCallback(
                PhGetPluginCallback(PluginInstance, PluginCallbackMenuItem),
                MenuItemCallback,
                NULL,
                &PluginMenuItemCallbackRegistration
                );

            PhAddSettings(settings, RTL_NUMBER_OF(settings));
        }
        break;
    }

    return TRUE;
}
