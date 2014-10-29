﻿////////////////////////////////////////////////////////////////////////////
//
//	zer0m0n DRIVER
//
//  Copyright 2013 Conix Security, Nicolas Correia, Adrien Chevalier
//
//  This file is part of zer0m0n.
//
//  Zer0m0n is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  Zer0m0n is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with Zer0m0n.  If not, see <http://www.gnu.org/licenses/>.
//
//
//	File :		main.c
//	Abstract :	Main function for Cuckoo Zero Driver
//	Revision : 	v1.0
//	Author :	Adrien Chevalier & Nicolas Correia
//	Email :		adrien.chevalier@conix.fr nicolas.correia@conix.fr
//	Date :		2013-12-26	  
//	Notes : 	
//		
/////////////////////////////////////////////////////////////////////////////
#include "main.h"
#include "comm.h"
#include "monitor.h"
#include "utils.h"
#include "reg.h"
#include "callbacks.h"
#include "hook.h"

// filter callbacks struct
FLT_REGISTRATION registration =
{
	sizeof(FLT_REGISTRATION),
	FLT_REGISTRATION_VERSION,
	FLTFL_REGISTRATION_DO_NOT_SUPPORT_SERVICE_STOP,		// block service stop
	NULL,
	NULL,
	UnregisterFilter,
	NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//	Description : initializes the driver and the filter port, registers driver and registry callbacks.
//
//	Parameters : 
//		_in_ PDRIVER_OBJECT pDriverObject :	Data structure used to represent the driver.
//		_in_ PUNICODE_STRING pRegistryPath :	Registry location where the information for the driver
//							was stored.
//	Return value :
//		NTSTATUS : STATUS_SUCCESS if the driver initialization has been well completed
//	Process :
//		Defines hidden / blocked processes names.
//		Creates the device driver and its symbolic link.
//		Sets IRP callbacks.
//		Creates filter communication port to send logs from the driver to the userland process.
//		Creates logs mutex.
//		Register image load and registry callbacks.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	PVOID adrFunc;
	PDEVICE_OBJECT pDeviceObject;
	UNICODE_STRING usDriverName;
	UNICODE_STRING filterPortName;
	UNICODE_STRING function;
	OBJECT_ATTRIBUTES objAttr;
	RTL_OSVERSIONINFOW osInfo;
	PSECURITY_DESCRIPTOR securityDescriptor;
	NTSTATUS status;
	ULONG i;

	// import some functions we will need
	RtlInitUnicodeString(&function, L"ZwQueryInformationThread");
	ZwQueryInformationThread = MmGetSystemRoutineAddress(&function);
	RtlInitUnicodeString(&function, L"ZwQueryInformationProcess");
	ZwQueryInformationProcess = MmGetSystemRoutineAddress(&function);
	RtlInitUnicodeString(&function, L"ZwQuerySystemInformation");
	ZwQuerySystemInformation = MmGetSystemRoutineAddress(&function);
	
	RtlInitUnicodeString(&usDriverName, L"\\Device\\DriverSSDT");
	RtlInitUnicodeString(&usDosDeviceName, L"\\DosDevices\\DriverSSDT"); 
	status = IoCreateDevice(pDriverObject, 0, &usDriverName, FILE_DEVICE_UNKNOWN, 0, FALSE, &pDeviceObject);
	if(!NT_SUCCESS(status))
		return status;
	
	status = IoCreateSymbolicLink(&usDosDeviceName, &usDriverName);
	if(!NT_SUCCESS(status))
		return status;
	
	pDeviceObject->Flags |= DO_BUFFERED_IO;
	pDeviceObject->Flags &= (~DO_DEVICE_INITIALIZING);
	for(i=0; i<IRP_MJ_MAXIMUM_FUNCTION; i++)
		pDriverObject->MajorFunction[i] = ioctl_NotSupported;
	pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ioctl_DeviceControl;
	
	monitored_process_list = NULL;
	hidden_process_list = NULL;
	monitored_handle_list = NULL;
	
	// initialize every function pointers to null
	oldNtMapViewOfSection = NULL;
	oldNtSetContextThread = NULL;
	oldNtCreateThread = NULL;
	oldNtQueueApcThread = NULL;
	oldNtCreateProcess = NULL;
	oldNtSystemDebugControl = NULL;
	oldNtCreateProcessEx = NULL;
	oldNtWriteVirtualMemory = NULL;
	oldNtDebugActiveProcess = NULL;
	oldNtOpenProcess = NULL;
	oldNtOpenThread = NULL;
	oldNtQuerySystemInformation = NULL;
	oldNtCreateFile = NULL;
	oldNtReadFile = NULL;
	oldNtWriteFile = NULL;
	oldNtDeleteFile = NULL;
	oldNtSetInformationFile = NULL;
	oldNtQueryInformationFile = NULL;
	oldNtCreateMutant = NULL;
	oldNtDeviceIoControlFile = NULL;
	oldNtTerminateProcess = NULL;
	oldNtDelayExecution = NULL;
	oldNtQueryValueKey = NULL;
	oldNtQueryAttributesFile = NULL;
	oldNtReadVirtualMemory = NULL;
	oldNtResumeThread = NULL;
	oldNtCreateSection = NULL;
	oldNtUserCallOneParam = NULL;
	oldNtUserCallNoParam = NULL;
	oldNtClose = NULL;
	oldNtOpenFile = NULL;
	
	#ifdef DEBUG
	DbgPrint("IOCTL_CUCKOO_PATH : 0x%08x\n", IOCTL_CUCKOO_PATH);
	#endif
	
	// get os version
	if(!NT_SUCCESS(RtlGetVersion(&osInfo)))
		return status;

	// check os version	
	if(osInfo.dwMajorVersion == 5 && osInfo.dwMinorVersion == 1) // xp 32 bits
	{
		is_xp = 1;
		#ifdef DEBUG
		DbgPrint("windows xp !\n");
		#endif
	}
	else if(osInfo.dwMajorVersion == 6 && osInfo.dwMinorVersion == 1) // win 7
	{
		is_xp = 0;
		#ifdef DEBUG
		DbgPrint("windows 7!\n");
		#endif
	}
	else
	{
		#ifdef DEBUG
		DbgPrint("error : not supported os\n");
		#endif
		return -1;
	}
	
   	status = FltRegisterFilter(pDriverObject,&registration,&filter);
	if(!NT_SUCCESS(status))
		return status;
	
	RtlInitUnicodeString(&filterPortName, L"\\FilterPort");
	status = FltBuildDefaultSecurityDescriptor(&securityDescriptor, FLT_PORT_ALL_ACCESS);
	if(!NT_SUCCESS(status))
		return status;
	
	InitializeObjectAttributes(&objAttr, &filterPortName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, securityDescriptor); 
	
	status = FltCreateCommunicationPort(filter, &serverPort, &objAttr, NULL, ConnectCallback, DisconnectCallback, NULL, 1);
	FltFreeSecurityDescriptor(securityDescriptor);
	if(!NT_SUCCESS(status))
		return status;
	
	KeInitializeMutex(&mutex, 0);
	
	status = CmRegisterCallback(regCallback, NULL, &cookie);
	if(!NT_SUCCESS(status))
		return status;
	
	status = PsSetLoadImageNotifyRoutine(imageCallback);
	if(!NT_SUCCESS(status))
		return status;
	
	KeServiceDescriptorTableShadow = getShadowTableAddress();
	
	if(!KeServiceDescriptorTableShadow)
	{
		#ifdef DEBUG
		DbgPrint("error : couldn't retrieve Shadow SSDT\n");
		#endif
		return STATUS_UNSUCCESSFUL;
	}

	
	pDriverObject->DriverUnload = Unload;
	return STATUS_SUCCESS;
}
 
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//	Description :
//		Driver unload callback. Removes hooks, callbacks, and communication stuff.
//	Parameters :
//	Process :
//		Removes hooks, callbacks, device driver symbolic link / device, and cleans the monitored
//		processes linked list.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
VOID Unload(PDRIVER_OBJECT pDriverObject)
{
	if(is_xp)
		unhook_ssdt_entries();
	else
		unhook_ssdt_entries_7();
			
	CmUnRegisterCallback(cookie);
	PsRemoveLoadImageNotifyRoutine(imageCallback);
	
	IoDeleteSymbolicLink(&usDosDeviceName);
	IoDeleteDevice(pDriverObject->DeviceObject);
	
	cleanMonitoredProcessList();	
	cleanHiddenProcessList();
	
	if (cuckooPath)
	{
		ExFreePool(cuckooPath);
	}
}
 
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
//  Description :
//	Unregisters the minifilter.
//	Parameters :
//	Return value :
//	Process :
//		Closes filter communication port and unregisters the filter.
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
NTSTATUS UnregisterFilter(FLT_FILTER_UNLOAD_FLAGS flags)
{
	FltCloseCommunicationPort(serverPort);
	
	if(filter!=NULL)
		FltUnregisterFilter(filter);
	
	return STATUS_FLT_DO_NOT_DETACH;
}
