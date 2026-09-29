/* NtCreateFile's failure status for a name that already exists.
 *
 * FILE_CREATE maps to CREATE_NEW, which fails on an existing file with
 * ERROR_FILE_EXISTS. A title creating a save file tells "already there" from
 * "the disk failed" by STATUS_OBJECT_NAME_COLLISION; STATUS_UNSUCCESSFUL sends
 * it down the failed-device path. */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static NTSTATUS create(const char *xbox_path, ULONG disposition, ULONG options)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    NTSTATUS st;

    name.Length = (USHORT)strlen(xbox_path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)xbox_path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0;
    st = xbox_NtCreateFile(&h, XBOX_GENERIC_READ | XBOX_GENERIC_WRITE, &oa, &iosb,
                           NULL, 0, 0, disposition, options);
    if (st == STATUS_SUCCESS)
        CloseHandle(h);
    return st;
}

int main(void)
{
    char root[MAX_PATH], dir[MAX_PATH], file[MAX_PATH];
    int failures = 0;
    NTSTATUS st;
    HANDLE h;

    GetTempPathA(MAX_PATH, root);
    sprintf(dir, "%sxbr-file-status-%lu", root, GetCurrentProcessId());
    if (!CreateDirectoryA(dir, NULL)) return 10;
    sprintf(file, "%s\\exists.dat", dir);
    h = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 11;
    CloseHandle(h);
    xbox_path_init(dir, NULL);

    st = create("D:\\exists.dat", XBOX_FILE_CREATE, XBOX_FILE_NON_DIRECTORY_FILE);
    if (st != STATUS_OBJECT_NAME_COLLISION) {
        fprintf(stderr, "FAIL: FILE_CREATE on an existing file: %08lX, want %08lX\n",
                (unsigned long)st, (unsigned long)STATUS_OBJECT_NAME_COLLISION);
        failures++;
    }
    /* Control: a name that is absent is still reported as not found. */
    st = create("D:\\absent.dat", XBOX_FILE_OPEN, XBOX_FILE_NON_DIRECTORY_FILE);
    if (st != STATUS_OBJECT_NAME_NOT_FOUND) {
        fprintf(stderr, "FAIL: FILE_OPEN on an absent file: %08lX, want %08lX\n",
                (unsigned long)st, (unsigned long)STATUS_OBJECT_NAME_NOT_FOUND);
        failures++;
    }

    DeleteFileA(file);
    RemoveDirectoryA(dir);
    if (failures) return 1;
    puts("PASS: NtCreateFile reports an existing file as a name collision");
    return 0;
}
