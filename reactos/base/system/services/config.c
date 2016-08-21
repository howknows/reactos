/*
 * PROJECT:     ReactOS Service Control Manager
 * LICENSE:     GPL - See COPYING in the top level directory
 * FILE:        base/system/services/config.c
 * PURPOSE:     Service configuration interface
 * COPYRIGHT:   Copyright 2005 Eric Kohl
 *
 */

/* INCLUDES *****************************************************************/

#include "services.h"
#include <ntsecapi.h>

#define NDEBUG
#include <debug.h>


/* FUNCTIONS *****************************************************************/


DWORD
ScmOpenServiceKey(LPWSTR lpServiceName,
                  REGSAM samDesired,
                  PHKEY phKey)
{
    HKEY hServicesKey = NULL;
    DWORD dwError;

    *phKey = NULL;

    dwError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"System\\CurrentControlSet\\Services",
                            0,
                            KEY_READ,
                            &hServicesKey);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    dwError = RegOpenKeyExW(hServicesKey,
                            lpServiceName,
                            0,
                            samDesired,
                            phKey);

    RegCloseKey(hServicesKey);

    return dwError;
}


DWORD
ScmCreateServiceKey(LPCWSTR lpServiceName,
                    REGSAM samDesired,
                    PHKEY phKey)
{
    HKEY hServicesKey = NULL;
    DWORD dwDisposition;
    DWORD dwError;

    *phKey = NULL;

    dwError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"System\\CurrentControlSet\\Services",
                            0,
                            KEY_READ | KEY_CREATE_SUB_KEY,
                            &hServicesKey);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    dwError = RegCreateKeyExW(hServicesKey,
                              lpServiceName,
                              0,
                              NULL,
                              REG_OPTION_NON_VOLATILE,
                              samDesired,
                              NULL,
                              phKey,
                              &dwDisposition);
#if 0
    if ((dwError == ERROR_SUCCESS) &&
        (dwDisposition == REG_OPENED_EXISTING_KEY))
    {
        RegCloseKey(*phKey);
        *phKey = NULL;
        dwError = ERROR_SERVICE_EXISTS;
    }
#endif

    RegCloseKey(hServicesKey);

    return dwError;
}



DWORD
ScmWriteDependencies(HKEY hServiceKey,
                     LPCWSTR lpDependencies,
                     DWORD dwDependenciesLength)
{
    DWORD dwError = ERROR_SUCCESS;
    SIZE_T cchGroupLength = 0;
    SIZE_T cchServiceLength = 0;
    SIZE_T cchLength;
    LPWSTR lpGroupDeps;
    LPWSTR lpServiceDeps;
    LPCWSTR lpSrc;
    LPWSTR lpDst;

    if (*lpDependencies == 0)
    {
        RegDeleteValueW(hServiceKey,
                       L"DependOnService");
        RegDeleteValueW(hServiceKey,
                       L"DependOnGroup");
    }
    else
    {
        lpGroupDeps = HeapAlloc(GetProcessHeap(),
                                HEAP_ZERO_MEMORY,
                                (dwDependenciesLength + 2) * sizeof(WCHAR));
        if (lpGroupDeps == NULL)
            return ERROR_NOT_ENOUGH_MEMORY;

        lpSrc = lpDependencies;
        lpDst = lpGroupDeps;
        while (*lpSrc != 0)
        {
            cchLength = wcslen(lpSrc) + 1;
            if (*lpSrc == SC_GROUP_IDENTIFIERW)
            {
                lpSrc++;
                cchLength--;
                cchGroupLength += cchLength;
                wcscpy(lpDst, lpSrc);
                lpDst = lpDst + cchLength;
            }

            lpSrc = lpSrc + cchLength;
        }
        *lpDst = 0;
        lpDst++;
        cchGroupLength++;

        lpSrc = lpDependencies;
        lpServiceDeps = lpDst;
        while (*lpSrc != 0)
        {
            cchLength = wcslen(lpSrc) + 1;
            if (*lpSrc != SC_GROUP_IDENTIFIERW)
            {
                cchServiceLength += cchLength;
                wcscpy(lpDst, lpSrc);
                lpDst = lpDst + cchLength;
            }

            lpSrc = lpSrc + cchLength;
        }
        *lpDst = 0;
        cchServiceLength++;

        if (cchGroupLength > 1)
        {
            dwError = RegSetValueExW(hServiceKey,
                                     L"DependOnGroup",
                                     0,
                                     REG_MULTI_SZ,
                                     (LPBYTE)lpGroupDeps,
                                     (DWORD)(cchGroupLength * sizeof(WCHAR)));
        }
        else
        {
            RegDeleteValueW(hServiceKey,
                            L"DependOnGroup");
        }

        if (dwError == ERROR_SUCCESS)
        {
            if (cchServiceLength > 1)
            {
                dwError = RegSetValueExW(hServiceKey,
                                         L"DependOnService",
                                         0,
                                         REG_MULTI_SZ,
                                         (LPBYTE)lpServiceDeps,
                                         (DWORD)(cchServiceLength * sizeof(WCHAR)));
            }
            else
            {
                RegDeleteValueW(hServiceKey,
                                L"DependOnService");
            }
        }

        HeapFree(GetProcessHeap(), 0, lpGroupDeps);
    }

    return dwError;
}


DWORD
ScmMarkServiceForDelete(PSERVICE pService)
{
    HKEY hServiceKey = NULL;
    DWORD dwValue = 1;
    DWORD dwError;

    DPRINT("ScmMarkServiceForDelete() called\n");

    dwError = ScmOpenServiceKey(pService->lpServiceName,
                                KEY_WRITE,
                                &hServiceKey);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    dwError = RegSetValueExW(hServiceKey,
                             L"DeleteFlag",
                             0,
                             REG_DWORD,
                             (LPBYTE)&dwValue,
                             sizeof(DWORD));

    RegCloseKey(hServiceKey);

    return dwError;
}


BOOL
ScmIsDeleteFlagSet(HKEY hServiceKey)
{
    DWORD dwError;
    DWORD dwType;
    DWORD dwFlag;
    DWORD dwSize = sizeof(DWORD);

    dwError = RegQueryValueExW(hServiceKey,
                               L"DeleteFlag",
                               0,
                               &dwType,
                               (LPBYTE)&dwFlag,
                               &dwSize);

    return (dwError == ERROR_SUCCESS);
}


DWORD
ScmReadString(HKEY hServiceKey,
              LPCWSTR lpValueName,
              LPWSTR *lpValue)
{
    DWORD dwError = 0;
    DWORD dwSize = 0;
    DWORD dwType = 0;
    LPWSTR ptr = NULL;
    LPWSTR expanded = NULL;

    *lpValue = NULL;

    dwError = RegQueryValueExW(hServiceKey,
                               lpValueName,
                               0,
                               &dwType,
                               NULL,
                               &dwSize);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    ptr = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize);
    if (ptr == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    dwError = RegQueryValueExW(hServiceKey,
                               lpValueName,
                               0,
                               &dwType,
                               (LPBYTE)ptr,
                               &dwSize);
    if (dwError != ERROR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, ptr);
        return dwError;
    }

    if (dwType == REG_EXPAND_SZ)
    {
        /* Expand the value... */
        dwSize = ExpandEnvironmentStringsW(ptr, NULL, 0);
        if (dwSize > 0)
        {
            expanded = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwSize * sizeof(WCHAR));
            if (expanded)
            {
                if (dwSize == ExpandEnvironmentStringsW(ptr, expanded, dwSize))
                {
                    *lpValue = expanded;
                    dwError = ERROR_SUCCESS;
                }
                else
                {
                    dwError = GetLastError();
                    HeapFree(GetProcessHeap(), 0, expanded);
                }
            }
            else
            {
                dwError = ERROR_NOT_ENOUGH_MEMORY;
            }
        }
        else
        {
            dwError = GetLastError();
        }

        HeapFree(GetProcessHeap(), 0, ptr);
    }
    else
    {
        *lpValue = ptr;
    }

    return dwError;
}


DWORD
ScmReadDependencies(HKEY hServiceKey,
                    LPWSTR *lpDependencies,
                    DWORD *lpdwDependenciesLength)
{
    LPWSTR lpGroups = NULL;
    LPWSTR lpServices = NULL;
    SIZE_T cchGroupsLength = 0;
    SIZE_T cchServicesLength = 0;
    LPWSTR lpSrc;
    LPWSTR lpDest;
    SIZE_T cchLength;
    SIZE_T cchTotalLength;

    *lpDependencies = NULL;
    *lpdwDependenciesLength = 0;

    /* Read the dependency values */
    ScmReadString(hServiceKey,
                  L"DependOnGroup",
                  &lpGroups);

    ScmReadString(hServiceKey,
                  L"DependOnService",
                  &lpServices);

    /* Leave, if there are no dependencies */
    if (lpGroups == NULL && lpServices == NULL)
        return ERROR_SUCCESS;

    /* Determine the total buffer size for the dependencies */
    if (lpGroups)
    {
        DPRINT("Groups:\n");
        lpSrc = lpGroups;
        while (*lpSrc != 0)
        {
            DPRINT("  %S\n", lpSrc);

            cchLength = wcslen(lpSrc) + 1;
            cchGroupsLength += cchLength + 1;

            lpSrc = lpSrc + cchLength;
        }
    }

    if (lpServices)
    {
        DPRINT("Services:\n");
        lpSrc = lpServices;
        while (*lpSrc != 0)
        {
            DPRINT("  %S\n", lpSrc);

            cchLength = wcslen(lpSrc) + 1;
            cchServicesLength += cchLength;

            lpSrc = lpSrc + cchLength;
        }
    }

    cchTotalLength = cchGroupsLength + cchServicesLength + 1;
    DPRINT("cchTotalLength: %lu\n", cchTotalLength);

    /* Allocate the common buffer for the dependencies */
    *lpDependencies = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cchTotalLength * sizeof(WCHAR));
    if (*lpDependencies == NULL)
    {
        if (lpGroups)
            HeapFree(GetProcessHeap(), 0, lpGroups);

        if (lpServices)
            HeapFree(GetProcessHeap(), 0, lpServices);

        return ERROR_NOT_ENOUGH_MEMORY;
    }

    /* Return the allocated buffer length in characters */
    *lpdwDependenciesLength = (DWORD)cchTotalLength;

    /* Copy the service dependencies into the common buffer */
    lpDest = *lpDependencies;
    if (lpServices)
    {
        memcpy(lpDest,
               lpServices,
               cchServicesLength * sizeof(WCHAR));

        lpDest = lpDest + cchServicesLength;
    }

    /* Copy the group dependencies into the common buffer */
    if (lpGroups)
    {
        lpSrc = lpGroups;
        while (*lpSrc != 0)
        {
            cchLength = wcslen(lpSrc) + 1;

            *lpDest = SC_GROUP_IDENTIFIERW;
            lpDest++;

            wcscpy(lpDest, lpSrc);

            lpDest = lpDest + cchLength;
            lpSrc = lpSrc + cchLength;
        }
    }

    /* Free the temporary buffers */
    if (lpGroups)
        HeapFree(GetProcessHeap(), 0, lpGroups);

    if (lpServices)
        HeapFree(GetProcessHeap(), 0, lpServices);

    return ERROR_SUCCESS;
}


DWORD
ScmSetServicePassword(
    IN PCWSTR pszServiceName,
    IN PCWSTR pszPassword)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    LSA_HANDLE PolicyHandle = NULL;
    UNICODE_STRING ServiceName = {0, 0, NULL};
    UNICODE_STRING Password;
    NTSTATUS Status;
    DWORD dwError = ERROR_SUCCESS;

    RtlZeroMemory(&ObjectAttributes, sizeof(OBJECT_ATTRIBUTES));

    Status = LsaOpenPolicy(NULL,
                           &ObjectAttributes,
                           POLICY_CREATE_SECRET,
                           &PolicyHandle);
    if (!NT_SUCCESS(Status))
        return RtlNtStatusToDosError(Status);

    ServiceName.Length = (wcslen(pszServiceName) + 4) * sizeof(WCHAR);
    ServiceName.MaximumLength = ServiceName.Length + sizeof(WCHAR);
    ServiceName.Buffer = HeapAlloc(GetProcessHeap(),
                                   HEAP_ZERO_MEMORY,
                                   ServiceName.MaximumLength);
    if (ServiceName.Buffer == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    wcscpy(ServiceName.Buffer, L"_SC_");
    wcscat(ServiceName.Buffer, pszServiceName);

    RtlInitUnicodeString(&Password, pszPassword);

    Status = LsaStorePrivateData(PolicyHandle,
                                 &ServiceName,
                                 pszPassword ? &Password : NULL);
    if (!NT_SUCCESS(Status))
    {
        dwError = RtlNtStatusToDosError(Status);
        goto done;
    }

done:
    if (ServiceName.Buffer != NULL)
        HeapFree(GetProcessHeap(), 0, ServiceName.Buffer);

    if (PolicyHandle != NULL)
        LsaClose(PolicyHandle);

    return dwError;
}


DWORD
ScmWriteSecurityDescriptor(
    _In_ HKEY hServiceKey,
    _In_ PSECURITY_DESCRIPTOR pSecurityDescriptor)
{
    HKEY hSecurityKey = NULL;
    DWORD dwDisposition;
    DWORD dwError;

    DPRINT("ScmWriteSecurityDescriptor(%p %p)\n", hServiceKey, pSecurityDescriptor);

    dwError = RegCreateKeyExW(hServiceKey,
                              L"Security",
                              0,
                              NULL,
                              REG_OPTION_NON_VOLATILE,
                              KEY_SET_VALUE,
                              NULL,
                              &hSecurityKey,
                              &dwDisposition);
    if (dwError != ERROR_SUCCESS)
        return dwError;

    dwError = RegSetValueExW(hSecurityKey,
                             L"Security",
                             0,
                             REG_BINARY,
                             (LPBYTE)pSecurityDescriptor,
                             RtlLengthSecurityDescriptor(pSecurityDescriptor));

    RegCloseKey(hSecurityKey);

    return dwError;
}


DWORD
ScmReadSecurityDescriptor(
    _In_ HKEY hServiceKey,
    _Out_ PSECURITY_DESCRIPTOR *ppSecurityDescriptor)
{
    PSECURITY_DESCRIPTOR pRelativeSD = NULL;
    HKEY hSecurityKey = NULL;
    DWORD dwBufferLength = 0;
    DWORD dwType;
    DWORD dwError;

    DPRINT("ScmReadSecurityDescriptor(%p %p)\n", hServiceKey, ppSecurityDescriptor);

    *ppSecurityDescriptor = NULL;

    dwError = RegOpenKeyExW(hServiceKey,
                            L"Security",
                            0,
                            KEY_QUERY_VALUE,
                            &hSecurityKey);
    if (dwError != ERROR_SUCCESS)
    {
        DPRINT("RegOpenKeyExW() failed (Error %lu)\n", dwError);

        /* Do not fail if the Security key does not exist */
        if (dwError == ERROR_FILE_NOT_FOUND)
            dwError = ERROR_SUCCESS;
        goto done;
    }

    dwError = RegQueryValueExW(hSecurityKey,
                               L"Security",
                               0,
                               &dwType,
                               NULL,
                               &dwBufferLength);
    if (dwError != ERROR_SUCCESS)
    {
        DPRINT("RegQueryValueExW() failed (Error %lu)\n", dwError);

        /* Do not fail if the Security value does not exist */
        if (dwError == ERROR_FILE_NOT_FOUND)
            dwError = ERROR_SUCCESS;
        goto done;
    }

    DPRINT("dwBufferLength: %lu\n", dwBufferLength);
    pRelativeSD = RtlAllocateHeap(RtlGetProcessHeap(),
                                  HEAP_ZERO_MEMORY,
                                  dwBufferLength);
    if (pRelativeSD == NULL)
    {
        return ERROR_OUTOFMEMORY;
    }

    DPRINT("pRelativeSD: %lu\n", pRelativeSD);
    dwError = RegQueryValueExW(hSecurityKey,
                               L"Security",
                               0,
                               &dwType,
                               (LPBYTE)pRelativeSD,
                               &dwBufferLength);
    if (dwError != ERROR_SUCCESS)
    {
        goto done;
    }

    *ppSecurityDescriptor = pRelativeSD;

done:
    if (dwError != ERROR_SUCCESS && pRelativeSD != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, pRelativeSD);

    if (hSecurityKey != NULL)
        RegCloseKey(hSecurityKey);

    return dwError;
}

/* EOF */
