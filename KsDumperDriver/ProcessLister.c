#include "NTUndocumented.h"
#include "ProcessLister.h"

static PSYSTEM_PROCESS_INFORMATION GetRawProcessList()
{
	ULONG bufferSize = 0;
	PVOID bufferPtr = NULL;

	if (ZwQuerySystemInformation(SystemProcessInformation, 0, bufferSize, &bufferSize) == STATUS_INFO_LENGTH_MISMATCH)
	{
		bufferPtr = ExAllocatePool(NonPagedPool, bufferSize);

		if (bufferPtr != NULL)
		{
			ZwQuerySystemInformation(SystemProcessInformation, bufferPtr, bufferSize, NULL);
		}
	}
	return (PSYSTEM_PROCESS_INFORMATION)bufferPtr;
}

static NTSTATUS DriverSleep(int ms)
{
	LARGE_INTEGER li;
	li.QuadPart = -10000;

	for (int i = 0; i < ms; i++)
	{
		KeDelayExecutionThread(KernelMode, FALSE, &li);
		return STATUS_SUCCESS;
	}
	return STATUS_UNSUCCESSFUL;
}

static ULONG CalculateListedProcessListSize(PSYSTEM_PROCESS_INFORMATION rawProcessList)
{
	int size = 0;

	while (rawProcessList->NextEntryOffset)
	{
		size += sizeof(PROCESS_SUMMARY);
		rawProcessList = (PSYSTEM_PROCESS_INFORMATION)(((CHAR*)rawProcessList) + rawProcessList->NextEntryOffset);
	}
	return size;
}

static PLDR_DATA_TABLE_ENTRY GetMainModuleEntry(PPEB64 peb)
{
	if (peb->Ldr)
	{
		if (!peb->Ldr->Initialized)
		{
			int initLoadCount = 0;

			while (!peb->Ldr->Initialized && initLoadCount++ < 4)
			{
				DriverSleep(250);
			}
		}

		if (peb->Ldr->Initialized)		
		{
			return CONTAINING_RECORD(peb->Ldr->InLoadOrderModuleList.Flink, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
		}
	}
	return NULL;
}

NTSTATUS GetProcessList(PVOID listedProcessBuffer, INT32 bufferSize, PINT32 requiredBufferSize, PINT32 processCount)
{
	PPROCESS_SUMMARY processSummary = (PPROCESS_SUMMARY)listedProcessBuffer;
	PSYSTEM_PROCESS_INFORMATION rawProcessList = GetRawProcessList();
	PVOID listHeadPointer = rawProcessList;
	*processCount = 0;

	if (rawProcessList)
	{
		int expectedBufferSize = CalculateListedProcessListSize(rawProcessList);

		if (!listedProcessBuffer || bufferSize < expectedBufferSize)
		{
			*requiredBufferSize = expectedBufferSize;
			return STATUS_INFO_LENGTH_MISMATCH;
		}

		while (rawProcessList->NextEntryOffset)
		{
			PEPROCESS targetProcess;
			PKAPC_STATE state = NULL;

			if (NT_SUCCESS(PsLookupProcessByProcessId(rawProcessList->UniqueProcessId, &targetProcess)))
			{
				PVOID mainModuleBase = NULL;
				PVOID mainModuleEntryPoint = NULL;
				UINT32 mainModuleImageSize = 0;
				PWCHAR mainModuleFileName = NULL;
				BOOLEAN isWow64 = 0;

				__try
				{
					KeStackAttachProcess(targetProcess, &state);

					__try
					{
						mainModuleBase = PsGetProcessSectionBaseAddress(targetProcess);
						PPEB64 peb = (PPEB64)PsGetProcessPeb(targetProcess);

						if (peb)
						{
							PLDR_DATA_TABLE_ENTRY mainModuleEntry = GetMainModuleEntry(peb);

							if (mainModuleEntry)
							{
								mainModuleEntryPoint = mainModuleEntry->EntryPoint;
								mainModuleImageSize = mainModuleEntry->SizeOfImage;
								isWow64 = IS_WOW64_PE(peb->ImageBaseAddress);

								mainModuleFileName = ExAllocatePool(NonPagedPool, 256 * sizeof(WCHAR));
								RtlZeroMemory(mainModuleFileName, mainModuleEntry->FullDllName.Length + sizeof(WCHAR));
								RtlCopyMemory(mainModuleFileName, mainModuleEntry->FullDllName.Buffer, mainModuleEntry->FullDllName.Length);
							}
						}
					}
					__except (GetExceptionCode())
					{
						DbgPrintEx(0, 0, "Peb Interaction Failed.\n");
					}
				}
				__finally
				{
					KeUnstackDetachProcess(&state);
				}

				if (mainModuleBase)
				{
					RtlCopyMemory(processSummary->MainModuleFileName, mainModuleFileName, 256 * sizeof(WCHAR));
					ExFreePool(mainModuleFileName);

					processSummary->ProcessId = rawProcessList->UniqueProcessId;
					processSummary->MainModuleBase = mainModuleBase;
					processSummary->MainModuleEntryPoint = mainModuleEntryPoint;
					processSummary->MainModuleImageSize = mainModuleImageSize;
					processSummary->WOW64 = isWow64;

					processSummary++;
					(*processCount)++;
				}
			}

			rawProcessList = (PSYSTEM_PROCESS_INFORMATION)(((CHAR*)rawProcessList) + rawProcessList->NextEntryOffset);
		}

		ExFreePool(listHeadPointer);
		return STATUS_SUCCESS;
	}
}