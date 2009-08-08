/*
 *  FreeLoader NTFS support
 *  Copyright (C) 2004  Filip Navara  <xnavara@volny.cz>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Limitations:
 * - No support for compressed files.
 * - May crash on corrupted filesystem.
 */

#include <freeldr.h>
#include <debug.h>

PNTFS_BOOTSECTOR NtfsBootSector;
ULONG NtfsClusterSize;
ULONG NtfsMftRecordSize;
ULONG NtfsIndexRecordSize;
ULONG NtfsDriveNumber;
ULONG NtfsSectorOfClusterZero;
PNTFS_MFT_RECORD NtfsMasterFileTable;
/* FIXME: NtfsMFTContext is never freed. */
PNTFS_ATTR_CONTEXT NtfsMFTContext;

static ULONGLONG NtfsGetAttributeSize(PNTFS_ATTR_RECORD AttrRecord)
{
    if (AttrRecord->IsNonResident)
        return AttrRecord->NonResident.DataSize;
    else
        return AttrRecord->Resident.ValueLength;
}

static PUCHAR NtfsDecodeRun(PUCHAR DataRun, LONGLONG *DataRunOffset, ULONGLONG *DataRunLength)
{
    UCHAR DataRunOffsetSize;
    UCHAR DataRunLengthSize;
    CHAR i;

    DataRunOffsetSize = (*DataRun >> 4) & 0xF;
    DataRunLengthSize = *DataRun & 0xF;
    *DataRunOffset = 0;
    *DataRunLength = 0;
    DataRun++;
    for (i = 0; i < DataRunLengthSize; i++)
    {
        *DataRunLength += *DataRun << (i << 3);
        DataRun++;
    }

    /* NTFS 3+ sparse files */
    if (DataRunOffsetSize == 0)
    {
        *DataRunOffset = -1;
    }
    else
    {
        for (i = 0; i < DataRunOffsetSize - 1; i++)
        {
            *DataRunOffset += *DataRun << (i << 3);
            DataRun++;
        }
        /* The last byte contains sign so we must process it different way. */
        *DataRunOffset = ((CHAR)(*(DataRun++)) << (i << 3)) + *DataRunOffset;
    }

    DPRINTM(DPRINT_FILESYSTEM, "DataRunOffsetSize: %x\n", DataRunOffsetSize);
    DPRINTM(DPRINT_FILESYSTEM, "DataRunLengthSize: %x\n", DataRunLengthSize);
    DPRINTM(DPRINT_FILESYSTEM, "DataRunOffset: %x\n", *DataRunOffset);
    DPRINTM(DPRINT_FILESYSTEM, "DataRunLength: %x\n", *DataRunLength);

    return DataRun;
}

static PNTFS_ATTR_CONTEXT NtfsPrepareAttributeContext(PNTFS_ATTR_RECORD AttrRecord)
{
    PNTFS_ATTR_CONTEXT Context;

    Context = MmHeapAlloc(FIELD_OFFSET(NTFS_ATTR_CONTEXT, Record) + AttrRecord->Length);
    RtlCopyMemory(&Context->Record, AttrRecord, AttrRecord->Length);
    if (AttrRecord->IsNonResident)
    {
    	LONGLONG DataRunOffset;
    	ULONGLONG DataRunLength;

        Context->CacheRun = (PUCHAR)&Context->Record + Context->Record.NonResident.MappingPairsOffset;
        Context->CacheRunOffset = 0;
        Context->CacheRun = NtfsDecodeRun(Context->CacheRun, &DataRunOffset, &DataRunLength);
        Context->CacheRunLength = DataRunLength;
        if (DataRunOffset != -1)
        {
            /* Normal run. */
            Context->CacheRunStartLCN =
            Context->CacheRunLastLCN = DataRunOffset;
        }
        else
        {
            /* Sparse run. */
            Context->CacheRunStartLCN = -1;
            Context->CacheRunLastLCN = 0;
        }
        Context->CacheRunCurrentOffset = 0;
    }

    return Context;
}

static VOID NtfsReleaseAttributeContext(PNTFS_ATTR_CONTEXT Context)
{
    MmHeapFree(Context);
}

/* FIXME: Optimize for multisector reads. */
static BOOLEAN NtfsDiskRead(ULONGLONG Offset, ULONGLONG Length, PCHAR Buffer)
{
    USHORT ReadLength;

    DPRINTM(DPRINT_FILESYSTEM, "NtfsDiskRead - Offset: %I64d Length: %I64d\n", Offset, Length);
    RtlZeroMemory((PCHAR)DISKREADBUFFER, 0x1000);

    /* I. Read partial first sector if needed */
    if (Offset % NtfsBootSector->BytesPerSector)
    {
        if (!MachDiskReadLogicalSectors(NtfsDriveNumber, NtfsSectorOfClusterZero + (Offset / NtfsBootSector->BytesPerSector), 1, (PCHAR)DISKREADBUFFER))
            return FALSE;
        ReadLength = min(Length, NtfsBootSector->BytesPerSector - (Offset % NtfsBootSector->BytesPerSector));
        RtlCopyMemory(Buffer, (PCHAR)DISKREADBUFFER + (Offset % NtfsBootSector->BytesPerSector), ReadLength);
        Buffer += ReadLength;
        Length -= ReadLength;
        Offset += ReadLength;
    }

    /* II. Read all complete 64-sector blocks. */
    while (Length >= (ULONGLONG)64 * (ULONGLONG)NtfsBootSector->BytesPerSector)
    {
        if (!MachDiskReadLogicalSectors(NtfsDriveNumber, NtfsSectorOfClusterZero + (Offset / NtfsBootSector->BytesPerSector), 64, (PCHAR)DISKREADBUFFER))
            return FALSE;
        RtlCopyMemory(Buffer, (PCHAR)DISKREADBUFFER, 64 * NtfsBootSector->BytesPerSector);
        Buffer += 64 * NtfsBootSector->BytesPerSector;
        Length -= 64 * NtfsBootSector->BytesPerSector;
        Offset += 64 * NtfsBootSector->BytesPerSector;
    }

    /* III. Read the rest of data */
    if (Length)
    {
        ReadLength = ((Length + NtfsBootSector->BytesPerSector - 1) / NtfsBootSector->BytesPerSector);
        if (!MachDiskReadLogicalSectors(NtfsDriveNumber, NtfsSectorOfClusterZero + (Offset / NtfsBootSector->BytesPerSector), ReadLength, (PCHAR)DISKREADBUFFER))
            return FALSE;
        RtlCopyMemory(Buffer, (PCHAR)DISKREADBUFFER, Length);
    }

    return TRUE;
}

static ULONGLONG NtfsReadAttribute(PNTFS_ATTR_CONTEXT Context, ULONGLONG Offset, PCHAR Buffer, ULONGLONG Length)
{
    ULONGLONG LastLCN;
    PUCHAR DataRun;
    LONGLONG DataRunOffset;
    ULONGLONG DataRunLength;
    LONGLONG DataRunStartLCN;
    ULONGLONG CurrentOffset;
    ULONGLONG ReadLength;
    ULONGLONG AlreadyRead;

    if (!Context->Record.IsNonResident)
    {
        if (Offset > Context->Record.Resident.ValueLength)
            return 0;
        if (Offset + Length > Context->Record.Resident.ValueLength)
            Length = Context->Record.Resident.ValueLength - Offset;
        RtlCopyMemory(Buffer, (PCHAR)&Context->Record + Context->Record.Resident.ValueOffset + Offset, Length);
        return Length;
    }

    /*
     * Non-resident attribute
     */

    /*
     * I. Find the corresponding start data run.
     */

    AlreadyRead = 0;

    if(Context->CacheRunOffset <= Offset && Offset < Context->CacheRunOffset + Context->CacheRunLength * NtfsClusterSize)
    {
        DataRun = Context->CacheRun;
        LastLCN = Context->CacheRunLastLCN;
        DataRunStartLCN = Context->CacheRunStartLCN;
        DataRunLength = Context->CacheRunLength;
        CurrentOffset = Context->CacheRunCurrentOffset;
    }
    else
    {
        LastLCN = 0;
        DataRun = (PUCHAR)&Context->Record + Context->Record.NonResident.MappingPairsOffset;
        CurrentOffset = 0;

        while (1)
        {
            DataRun = NtfsDecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunOffset != -1)
            {
                /* Normal data run. */
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
            {
                /* Sparse data run. */
                DataRunStartLCN = -1;
            }

            if (Offset >= CurrentOffset &&
                Offset < CurrentOffset + (DataRunLength * NtfsClusterSize))
            {
                break;
            }

            if (*DataRun == 0)
            {
                return AlreadyRead;
            }

            CurrentOffset += DataRunLength * NtfsClusterSize;
        }
    }

    /*
     * II. Go through the run list and read the data
     */

    ReadLength = min(DataRunLength * NtfsClusterSize - (Offset - CurrentOffset), Length);
    if (DataRunStartLCN == -1)
    RtlZeroMemory(Buffer, ReadLength);
    if (NtfsDiskRead(DataRunStartLCN * NtfsClusterSize + Offset - CurrentOffset, ReadLength, Buffer))
    {
        Length -= ReadLength;
        Buffer += ReadLength;
        AlreadyRead += ReadLength;

        if (ReadLength == DataRunLength * NtfsClusterSize - (Offset - CurrentOffset))
        {
            CurrentOffset += DataRunLength * NtfsClusterSize;
            DataRun = NtfsDecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunLength != (ULONGLONG)-1)
            {
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
                DataRunStartLCN = -1;

            if (*DataRun == 0)
                return AlreadyRead;
        }

        while (Length > 0)
        {
            ReadLength = min(DataRunLength * NtfsClusterSize, Length);
            if (DataRunStartLCN == -1)
                RtlZeroMemory(Buffer, ReadLength);
            else if (!NtfsDiskRead(DataRunStartLCN * NtfsClusterSize, ReadLength, Buffer))
                break;

            Length -= ReadLength;
            Buffer += ReadLength;
            AlreadyRead += ReadLength;

            /* We finished this request, but there still data in this data run. */
            if (Length == 0 && ReadLength != DataRunLength * NtfsClusterSize)
                break;

            /*
             * Go to next run in the list.
             */

            if (*DataRun == 0)
                break;
            CurrentOffset += DataRunLength * NtfsClusterSize;
            DataRun = NtfsDecodeRun(DataRun, &DataRunOffset, &DataRunLength);
            if (DataRunOffset != -1)
            {
                /* Normal data run. */
                DataRunStartLCN = LastLCN + DataRunOffset;
                LastLCN = DataRunStartLCN;
            }
            else
            {
                /* Sparse data run. */
                DataRunStartLCN = -1;
            }
        } /* while */

    } /* if Disk */

    Context->CacheRun = DataRun;
    Context->CacheRunOffset = Offset + AlreadyRead;
    Context->CacheRunStartLCN = DataRunStartLCN;
    Context->CacheRunLength = DataRunLength;
    Context->CacheRunLastLCN = LastLCN;
    Context->CacheRunCurrentOffset = CurrentOffset;

    return AlreadyRead;
}

static PNTFS_ATTR_CONTEXT NtfsFindAttributeHelper(PNTFS_ATTR_RECORD AttrRecord, PNTFS_ATTR_RECORD AttrRecordEnd, ULONG Type, const WCHAR *Name, ULONG NameLength)
{
    while (AttrRecord < AttrRecordEnd)
    {
        if (AttrRecord->Type == NTFS_ATTR_TYPE_END)
            break;

        if (AttrRecord->Type == NTFS_ATTR_TYPE_ATTRIBUTE_LIST)
        {
            PNTFS_ATTR_CONTEXT Context;
            PNTFS_ATTR_CONTEXT ListContext;
            PVOID ListBuffer;
            ULONGLONG ListSize;
            PNTFS_ATTR_RECORD ListAttrRecord;
            PNTFS_ATTR_RECORD ListAttrRecordEnd;

            ListContext = NtfsPrepareAttributeContext(AttrRecord);

            ListSize = NtfsGetAttributeSize(&ListContext->Record);
            ListBuffer = MmHeapAlloc(ListSize);

            ListAttrRecord = (PNTFS_ATTR_RECORD)ListBuffer;
            ListAttrRecordEnd = (PNTFS_ATTR_RECORD)((PCHAR)ListBuffer + ListSize);

            if (NtfsReadAttribute(ListContext, 0, ListBuffer, ListSize) == ListSize)
            {
                Context = NtfsFindAttributeHelper(ListAttrRecord, ListAttrRecordEnd,
                                                  Type, Name, NameLength);

                NtfsReleaseAttributeContext(ListContext);
                MmHeapFree(ListBuffer);

                if (Context != NULL)
                    return Context;
            }
        }

        if (AttrRecord->Type == Type)
        {
            if (AttrRecord->NameLength == NameLength)
            {
                PWCHAR AttrName;

                AttrName = (PWCHAR)((PCHAR)AttrRecord + AttrRecord->NameOffset);
                if (RtlEqualMemory(AttrName, Name, NameLength << 1))
                {
                    /* Found it, fill up the context and return. */
                    return NtfsPrepareAttributeContext(AttrRecord);
                }
            }
        }

        AttrRecord = (PNTFS_ATTR_RECORD)((PCHAR)AttrRecord + AttrRecord->Length);
    }

    return NULL;
}

static PNTFS_ATTR_CONTEXT NtfsFindAttribute(PNTFS_MFT_RECORD MftRecord, ULONG Type, const WCHAR *Name)
{
    PNTFS_ATTR_RECORD AttrRecord;
    PNTFS_ATTR_RECORD AttrRecordEnd;
    ULONG NameLength;

    AttrRecord = (PNTFS_ATTR_RECORD)((PCHAR)MftRecord + MftRecord->AttributesOffset);
    AttrRecordEnd = (PNTFS_ATTR_RECORD)((PCHAR)MftRecord + NtfsMftRecordSize);
    for (NameLength = 0; Name[NameLength] != 0; NameLength++)
        ;

    return NtfsFindAttributeHelper(AttrRecord, AttrRecordEnd, Type, Name, NameLength);
}

static BOOLEAN NtfsFixupRecord(PNTFS_RECORD Record)
{
    USHORT *USA;
    USHORT USANumber;
    USHORT USACount;
    USHORT *Block;

    USA = (USHORT*)((PCHAR)Record + Record->USAOffset);
    USANumber = *(USA++);
    USACount = Record->USACount - 1; /* Exclude the USA Number. */
    Block = (USHORT*)((PCHAR)Record + NtfsBootSector->BytesPerSector - 2);

    while (USACount)
    {
        if (*Block != USANumber)
            return FALSE;
        *Block = *(USA++);
        Block = (USHORT*)((PCHAR)Block + NtfsBootSector->BytesPerSector);
        USACount--;
    }

    return TRUE;
}

static BOOLEAN NtfsReadMftRecord(ULONG MFTIndex, PNTFS_MFT_RECORD Buffer)
{
    ULONGLONG BytesRead;

    BytesRead = NtfsReadAttribute(NtfsMFTContext, MFTIndex * NtfsMftRecordSize, (PCHAR)Buffer, NtfsMftRecordSize);
    if (BytesRead != NtfsMftRecordSize)
        return FALSE;

    /* Apply update sequence array fixups. */
    return NtfsFixupRecord((PNTFS_RECORD)Buffer);
}

#if DBG
VOID NtfsPrintFile(PNTFS_INDEX_ENTRY IndexEntry)
{
    PWCHAR FileName;
    UCHAR FileNameLength;
    CHAR AnsiFileName[256];
    UCHAR i;

    FileName = IndexEntry->FileName.FileName;
    FileNameLength = IndexEntry->FileName.FileNameLength;

    for (i = 0; i < FileNameLength; i++)
        AnsiFileName[i] = FileName[i];
    AnsiFileName[i] = 0;

    DPRINTM(DPRINT_FILESYSTEM, "- %s (%x)\n", AnsiFileName, IndexEntry->Data.Directory.IndexedFile);
}
#endif

static BOOLEAN NtfsCompareFileName(PCHAR FileName, PNTFS_INDEX_ENTRY IndexEntry)
{
    PWCHAR EntryFileName;
    UCHAR EntryFileNameLength;
    UCHAR i;

    EntryFileName = IndexEntry->FileName.FileName;
    EntryFileNameLength = IndexEntry->FileName.FileNameLength;

#if DBG
    NtfsPrintFile(IndexEntry);
#endif

    if (strlen(FileName) != EntryFileNameLength)
        return FALSE;

    /* Do case-sensitive compares for Posix file names. */
    if (IndexEntry->FileName.FileNameType == NTFS_FILE_NAME_POSIX)
    {
        for (i = 0; i < EntryFileNameLength; i++)
            if (EntryFileName[i] != FileName[i])
                return FALSE;
    }
    else
    {
        for (i = 0; i < EntryFileNameLength; i++)
            if (tolower(EntryFileName[i]) != tolower(FileName[i]))
                return FALSE;
    }

    return TRUE;
}

static BOOLEAN NtfsFindMftRecord(ULONG MFTIndex, PCHAR FileName, ULONG *OutMFTIndex)
{
    PNTFS_MFT_RECORD MftRecord;
    ULONG Magic;
    PNTFS_ATTR_CONTEXT IndexRootCtx;
    PNTFS_ATTR_CONTEXT IndexBitmapCtx;
    PNTFS_ATTR_CONTEXT IndexAllocationCtx;
    PNTFS_INDEX_ROOT IndexRoot;
    ULONGLONG BitmapDataSize;
    ULONGLONG IndexAllocationSize;
    PCHAR BitmapData;
    PCHAR IndexRecord;
    PNTFS_INDEX_ENTRY IndexEntry, IndexEntryEnd;
    ULONG RecordOffset;
    ULONG IndexBlockSize;

    MftRecord = MmHeapAlloc(NtfsMftRecordSize);
    if (MftRecord == NULL)
    {
        return FALSE;
    }

    if (NtfsReadMftRecord(MFTIndex, MftRecord))
    {
        Magic = MftRecord->Magic;

        IndexRootCtx = NtfsFindAttribute(MftRecord, NTFS_ATTR_TYPE_INDEX_ROOT, L"$I30");
        if (IndexRootCtx == NULL)
        {
            MmHeapFree(MftRecord);
            return FALSE;
        }

        IndexRecord = MmHeapAlloc(NtfsIndexRecordSize);
        if (IndexRecord == NULL)
        {
            MmHeapFree(MftRecord);
            return FALSE;
        }

        NtfsReadAttribute(IndexRootCtx, 0, IndexRecord, NtfsIndexRecordSize);
        IndexRoot = (PNTFS_INDEX_ROOT)IndexRecord;
        IndexEntry = (PNTFS_INDEX_ENTRY)((PCHAR)&IndexRoot->IndexHeader + IndexRoot->IndexHeader.EntriesOffset);
        /* Index root is always resident. */
        IndexEntryEnd = (PNTFS_INDEX_ENTRY)(IndexRecord + IndexRootCtx->Record.Resident.ValueLength);
        NtfsReleaseAttributeContext(IndexRootCtx);

        DPRINTM(DPRINT_FILESYSTEM, "NtfsIndexRecordSize: %x IndexBlockSize: %x\n", NtfsIndexRecordSize, IndexRoot->IndexBlockSize);

        while (IndexEntry < IndexEntryEnd &&
               !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
        {
            if (NtfsCompareFileName(FileName, IndexEntry))
            {
                *OutMFTIndex = IndexEntry->Data.Directory.IndexedFile;
                MmHeapFree(IndexRecord);
                MmHeapFree(MftRecord);
                return TRUE;
            }
	    IndexEntry = (PNTFS_INDEX_ENTRY)((PCHAR)IndexEntry + IndexEntry->Length);
        }

        if (IndexRoot->IndexHeader.Flags & NTFS_LARGE_INDEX)
        {
            DPRINTM(DPRINT_FILESYSTEM, "Large Index!\n");

            IndexBlockSize = IndexRoot->IndexBlockSize;

            IndexBitmapCtx = NtfsFindAttribute(MftRecord, NTFS_ATTR_TYPE_BITMAP, L"$I30");
            if (IndexBitmapCtx == NULL)
            {
                DPRINTM(DPRINT_FILESYSTEM, "Corrupted filesystem!\n");
                MmHeapFree(MftRecord);
                return FALSE;
            }
            BitmapDataSize = NtfsGetAttributeSize(&IndexBitmapCtx->Record);
            DPRINTM(DPRINT_FILESYSTEM, "BitmapDataSize: %x\n", BitmapDataSize);
            BitmapData = MmHeapAlloc(BitmapDataSize);
            if (BitmapData == NULL)
            {
                MmHeapFree(IndexRecord);
                MmHeapFree(MftRecord);
                return FALSE;
            }
            NtfsReadAttribute(IndexBitmapCtx, 0, BitmapData, BitmapDataSize);
            NtfsReleaseAttributeContext(IndexBitmapCtx);

            IndexAllocationCtx = NtfsFindAttribute(MftRecord, NTFS_ATTR_TYPE_INDEX_ALLOCATION, L"$I30");
            if (IndexAllocationCtx == NULL)
            {
                DPRINTM(DPRINT_FILESYSTEM, "Corrupted filesystem!\n");
                MmHeapFree(BitmapData);
                MmHeapFree(IndexRecord);
                MmHeapFree(MftRecord);
                return FALSE;
            }
            IndexAllocationSize = NtfsGetAttributeSize(&IndexAllocationCtx->Record);

            RecordOffset = 0;

            for (;;)
            {
                DPRINTM(DPRINT_FILESYSTEM, "RecordOffset: %x IndexAllocationSize: %x\n", RecordOffset, IndexAllocationSize);
                for (; RecordOffset < IndexAllocationSize;)
                {
                    UCHAR Bit = 1 << ((RecordOffset / IndexBlockSize) & 7);
                    ULONG Byte = (RecordOffset / IndexBlockSize) >> 3;
                    if ((BitmapData[Byte] & Bit))
                        break;
                    RecordOffset += IndexBlockSize;
                }

                if (RecordOffset >= IndexAllocationSize)
                {
                    break;
                }

                NtfsReadAttribute(IndexAllocationCtx, RecordOffset, IndexRecord, IndexBlockSize);

                if (!NtfsFixupRecord((PNTFS_RECORD)IndexRecord))
                {
                    break;
                }

                /* FIXME */
                IndexEntry = (PNTFS_INDEX_ENTRY)(IndexRecord + 0x18 + *(USHORT *)(IndexRecord + 0x18));
	        IndexEntryEnd = (PNTFS_INDEX_ENTRY)(IndexRecord + IndexBlockSize);

                while (IndexEntry < IndexEntryEnd &&
                       !(IndexEntry->Flags & NTFS_INDEX_ENTRY_END))
                {
                    if (NtfsCompareFileName(FileName, IndexEntry))
                    {
                        DPRINTM(DPRINT_FILESYSTEM, "File found\n");
                        *OutMFTIndex = IndexEntry->Data.Directory.IndexedFile;
                        MmHeapFree(BitmapData);
                        MmHeapFree(IndexRecord);
                        MmHeapFree(MftRecord);
                        NtfsReleaseAttributeContext(IndexAllocationCtx);
                        return TRUE;
                    }
                    IndexEntry = (PNTFS_INDEX_ENTRY)((PCHAR)IndexEntry + IndexEntry->Length);
                }

                RecordOffset += IndexBlockSize;
            }

            NtfsReleaseAttributeContext(IndexAllocationCtx);
            MmHeapFree(BitmapData);
        }

        MmHeapFree(IndexRecord);
    }
    else
    {
        DPRINTM(DPRINT_FILESYSTEM, "Can't read MFT record\n");
    }
    MmHeapFree(MftRecord);

    return FALSE;
}

static BOOLEAN NtfsLookupFile(PCSTR FileName, PNTFS_MFT_RECORD MftRecord, PNTFS_ATTR_CONTEXT *DataContext)
{
    ULONG NumberOfPathParts;
    CHAR PathPart[261];
    ULONG CurrentMFTIndex;
    UCHAR i;

    DPRINTM(DPRINT_FILESYSTEM, "NtfsLookupFile() FileName = %s\n", FileName);

    CurrentMFTIndex = NTFS_FILE_ROOT;
    NumberOfPathParts = FsGetNumPathParts(FileName);
    for (i = 0; i < NumberOfPathParts; i++)
    {
        FsGetFirstNameFromPath(PathPart, FileName);

        for (; (*FileName != '\\') && (*FileName != '/') && (*FileName != '\0'); FileName++)
            ;
        FileName++;

        DPRINTM(DPRINT_FILESYSTEM, "- Lookup: %s\n", PathPart);
        if (!NtfsFindMftRecord(CurrentMFTIndex, PathPart, &CurrentMFTIndex))
        {
            DPRINTM(DPRINT_FILESYSTEM, "- Failed\n");
            return FALSE;
        }
        DPRINTM(DPRINT_FILESYSTEM, "- Lookup: %x\n", CurrentMFTIndex);
    }

    if (!NtfsReadMftRecord(CurrentMFTIndex, MftRecord))
    {
        DPRINTM(DPRINT_FILESYSTEM, "NtfsLookupFile: Can't read MFT record\n");
        return FALSE;
    }

    *DataContext = NtfsFindAttribute(MftRecord, NTFS_ATTR_TYPE_DATA, L"");
    if (*DataContext == NULL)
    {
        DPRINTM(DPRINT_FILESYSTEM, "NtfsLookupFile: Can't find data attribute\n");
        return FALSE;
    }

    return TRUE;
}

BOOLEAN NtfsOpenVolume(UCHAR DriveNumber, ULONGLONG VolumeStartSector, ULONGLONG PartitionSectorCount)
{
    NtfsBootSector = (PNTFS_BOOTSECTOR)DISKREADBUFFER;

    DPRINTM(DPRINT_FILESYSTEM, "NtfsOpenVolume() DriveNumber = 0x%x VolumeStartSector = 0x%x\n", DriveNumber, VolumeStartSector);

    if (!MachDiskReadLogicalSectors(DriveNumber, VolumeStartSector, 1, (PCHAR)DISKREADBUFFER))
    {
        FileSystemError("Failed to read the boot sector.");
        return FALSE;
    }

    if (!RtlEqualMemory(NtfsBootSector->SystemId, "NTFS", 4))
    {
        FileSystemError("Invalid NTFS signature.");
        return FALSE;
    }

    NtfsBootSector = MmHeapAlloc(NtfsBootSector->BytesPerSector);
    if (NtfsBootSector == NULL)
    {
        return FALSE;
    }

    RtlCopyMemory(NtfsBootSector, (PCHAR)DISKREADBUFFER, ((PNTFS_BOOTSECTOR)DISKREADBUFFER)->BytesPerSector);

    NtfsClusterSize = NtfsBootSector->SectorsPerCluster * NtfsBootSector->BytesPerSector;
    if (NtfsBootSector->ClustersPerMftRecord > 0)
        NtfsMftRecordSize = NtfsBootSector->ClustersPerMftRecord * NtfsClusterSize;
    else
        NtfsMftRecordSize = 1 << (-NtfsBootSector->ClustersPerMftRecord);
    if (NtfsBootSector->ClustersPerIndexRecord > 0)
        NtfsIndexRecordSize = NtfsBootSector->ClustersPerIndexRecord * NtfsClusterSize;
    else
        NtfsIndexRecordSize = 1 << (-NtfsBootSector->ClustersPerIndexRecord);

    DPRINTM(DPRINT_FILESYSTEM, "NtfsClusterSize: 0x%x\n", NtfsClusterSize);
    DPRINTM(DPRINT_FILESYSTEM, "ClustersPerMftRecord: %d\n", NtfsBootSector->ClustersPerMftRecord);
    DPRINTM(DPRINT_FILESYSTEM, "ClustersPerIndexRecord: %d\n", NtfsBootSector->ClustersPerIndexRecord);
    DPRINTM(DPRINT_FILESYSTEM, "NtfsMftRecordSize: 0x%x\n", NtfsMftRecordSize);
    DPRINTM(DPRINT_FILESYSTEM, "NtfsIndexRecordSize: 0x%x\n", NtfsIndexRecordSize);

    NtfsDriveNumber = DriveNumber;
    NtfsSectorOfClusterZero = VolumeStartSector;

    DPRINTM(DPRINT_FILESYSTEM, "Reading MFT index...\n");
    if (!MachDiskReadLogicalSectors(DriveNumber,
                                NtfsSectorOfClusterZero +
                                (NtfsBootSector->MftLocation * NtfsBootSector->SectorsPerCluster),
                                NtfsMftRecordSize / NtfsBootSector->BytesPerSector, (PCHAR)DISKREADBUFFER))
    {
        FileSystemError("Failed to read the Master File Table record.");
        return FALSE;
    }

    NtfsMasterFileTable = MmHeapAlloc(NtfsMftRecordSize);
    if (NtfsMasterFileTable == NULL)
    {
        MmHeapFree(NtfsBootSector);
        return FALSE;
    }

    RtlCopyMemory(NtfsMasterFileTable, (PCHAR)DISKREADBUFFER, NtfsMftRecordSize);

    DPRINTM(DPRINT_FILESYSTEM, "Searching for DATA attribute...\n");
    NtfsMFTContext = NtfsFindAttribute(NtfsMasterFileTable, NTFS_ATTR_TYPE_DATA, L"");
    if (NtfsMFTContext == NULL)
    {
        FileSystemError("Can't find data attribute for Master File Table.");
        return FALSE;
    }

    return TRUE;
}

LONG NtfsClose(ULONG FileId)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);

    NtfsReleaseAttributeContext(FileHandle->DataContext);
    MmHeapFree(FileHandle);

    return ESUCCESS;
}

LONG NtfsGetFileInformation(ULONG FileId, FILEINFORMATION* Information)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);

    RtlZeroMemory(Information, sizeof(FILEINFORMATION));
    Information->EndingAddress.LowPart = (ULONG)NtfsGetAttributeSize(&FileHandle->DataContext->Record);
    Information->CurrentAddress.LowPart = FileHandle->Offset;

    DPRINTM(DPRINT_FILESYSTEM, "NtfsGetFileInformation() FileSize = %d\n",
        Information->EndingAddress.LowPart);
    DPRINTM(DPRINT_FILESYSTEM, "NtfsGetFileInformation() FilePointer = %d\n",
        Information->CurrentAddress.LowPart);

    return ESUCCESS;
}

LONG NtfsOpen(CHAR* Path, OPENMODE OpenMode, ULONG* FileId)
{
    PNTFS_FILE_HANDLE FileHandle;
    PNTFS_MFT_RECORD MftRecord;
    ULONG DeviceId;

    //
    // Check parameters
    //
    if (OpenMode != OpenReadOnly)
        return EACCES;

    //
    // Get underlying device
    //
    DeviceId = FsGetDeviceId(*FileId);

    DPRINTM(DPRINT_FILESYSTEM, "NtfsOpen() FileName = %s\n", Path);

    //
    // Allocate file structure
    //
    FileHandle = MmHeapAlloc(sizeof(NTFS_FILE_HANDLE) + NtfsMftRecordSize);
    if (!FileHandle)
    {
        return ENOMEM;
    }
    RtlZeroMemory(FileHandle, sizeof(NTFS_FILE_HANDLE) + NtfsMftRecordSize);

    //
    // Search file entry
    //
    MftRecord = (PNTFS_MFT_RECORD)(FileHandle + 1);
    if (!NtfsLookupFile(Path, MftRecord, &FileHandle->DataContext))
    {
        MmHeapFree(FileHandle);
        return ENOENT;
    }

    return ESUCCESS;
}

LONG NtfsRead(ULONG FileId, VOID* Buffer, ULONG N, ULONG* Count)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);
    ULONGLONG BytesRead64;

    //
    // Read file
    //
    BytesRead64 = NtfsReadAttribute(FileHandle->DataContext, FileHandle->Offset, Buffer, N);
    *Count = (ULONG)BytesRead64;

    //
    // Check for success
    //
    if (BytesRead64 > 0)
        return ESUCCESS;
    else
        return EIO;
}

LONG NtfsSeek(ULONG FileId, LARGE_INTEGER* Position, SEEKMODE SeekMode)
{
    PNTFS_FILE_HANDLE FileHandle = FsGetDeviceSpecific(FileId);

    DPRINTM(DPRINT_FILESYSTEM, "NtfsSeek() NewFilePointer = %lu\n", Position->LowPart);

    if (SeekMode != SeekAbsolute)
        return EINVAL;
    if (Position->HighPart != 0)
        return EINVAL;
    if (Position->LowPart >= (ULONG)NtfsGetAttributeSize(&FileHandle->DataContext->Record))
        return EINVAL;

    FileHandle->Offset = Position->LowPart;
    return ESUCCESS;
}

const DEVVTBL NtfsFuncTable =
{
    NtfsClose,
    NtfsGetFileInformation,
    NtfsOpen,
    NtfsRead,
    NtfsSeek,
};

const DEVVTBL* NtfsMount(ULONG DeviceId)
{
    NTFS_BOOTSECTOR BootSector;
    LARGE_INTEGER Position;
    ULONG Count;
    LONG ret;

    //
    // Read the BootSector
    //
    Position.HighPart = 0;
    Position.LowPart = 0;
    ret = ArcSeek(DeviceId, &Position, SeekAbsolute);
    if (ret != ESUCCESS)
        return NULL;
    ret = ArcRead(DeviceId, &BootSector, sizeof(BootSector), &Count);
    if (ret != ESUCCESS || Count != sizeof(BootSector))
        return NULL;

    //
    // Check if BootSector is valid. If yes, return NTFS function table
    //
    if (RtlEqualMemory(BootSector.SystemId, "NTFS", 4))
    {
        //
        // Compatibility hack as long as FS is not using underlying device DeviceId
        //
        ULONG DriveNumber;
        ULONGLONG StartSector;
        ULONGLONG SectorCount;
        int Type;
        if (!MachDiskGetBootVolume(&DriveNumber, &StartSector, &SectorCount, &Type))
            return NULL;
        NtfsOpenVolume(DriveNumber, StartSector, SectorCount);
        return &NtfsFuncTable;
    }
    else
        return NULL;
}
