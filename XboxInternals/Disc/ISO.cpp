#include "ISO.h"
#include "IO/IsoIO.h"

ISO::ISO(BaseIO *io) :
    freeIO(false), didReadFileListing(false), titleName(L"")
{
    ParseISO();
}

ISO::ISO(std::string filePath) :
    io(new BigFileIO(filePath)), freeIO(true), didReadFileListing(false), titleName(L"")
{
    ParseISO();
}

ISO::~ISO()
{
    if (freeIO)
        delete io;
}

UINT64 ISO::SectorToAddress(DWORD sector)
{
    return (UINT64)sector * ISO_SECTOR_SIZE + gdfxHeaderAddress - ISO_XGD1_ADDRESS;
}

void ISO::GetFileListing()
{
    if (!didReadFileListing)
    {
        didReadFileListing = true;
        ReadFileListing(&root, gdfxHeader.rootSector, gdfxHeader.rootSize, "");
    }
}

void ISO::ExtractFile(std::string outDirectory, const GdfxFileEntry *fileEntry, void (*progress)(void*, DWORD, DWORD), void *arg)
{
    DWORD curProgress = 0;
    DWORD totalReads = fileEntry->size / ISO_COPY_BUFFER_SIZE + 1;
    ExtractFileHelper(outDirectory, fileEntry, progress, arg, &curProgress, totalReads);
}

void ISO::ExtractFile(std::string outDirectory, std::string filePath, void (*progress)(void *, DWORD, DWORD), void *arg)
{
    GdfxFileEntry *entry = GetFileEntry(filePath);
    ExtractFile(outDirectory, entry, progress, arg);
}

void ISO::ExtractAll(std::string outDirectory, void (*progress)(void*, DWORD, DWORD), void *arg)
{
    DWORD curProgress = 0;
    ExtractAllHelper(outDirectory, &root, progress, arg, &curProgress, GetTotalCopyIterations(&root));
}

GdfxFileEntry* ISO::GetFileEntry(std::string filePath)
{
    std::vector<GdfxFileEntry> *curDirectory = &root;

    std::string normalizedPath = Utils::NormalizeFilePath(filePath);
    size_t separatorIndex = normalizedPath.find('\\');

    // remove the trailing \ if it's there
    if (normalizedPath.at(normalizedPath.size() - 1) == '\\')
        normalizedPath = normalizedPath.substr(0, normalizedPath.size() - 1);

    // iterate over all the directories in the path
    GdfxFileEntry *curDirectoryEntry = NULL;
    GdfxFileEntry *foundFileEntry = NULL;
    while (true)
    {
        std::string entryName = normalizedPath.substr(0, separatorIndex);

        // find the entry in the current directory
        for (size_t i = 0; i < curDirectory->size(); i++)
        {
            GdfxFileEntry *curEntry = &(curDirectory->at(i));
            if (curEntry->name == entryName)
            {
                // if this isn't the last entry in the chain then we need to continue following the chain
                if (separatorIndex != std::string::npos)
                {
                    curDirectoryEntry = curEntry;
                    break;
                }
                else
                {
                    foundFileEntry = curEntry;
                    goto EndDirectoriesInPathLoop;
                }
            }
        }

        if (curDirectoryEntry == NULL)
            throw std::string("ISO: Unable to find file " + filePath);

        // remove the current directory from the path and point the new current directory at the next one
        normalizedPath = normalizedPath.substr(separatorIndex + 1);
        curDirectory = &(curDirectoryEntry->files);

        separatorIndex = normalizedPath.find('\\');
    }

EndDirectoriesInPathLoop:

    return foundFileEntry;
}

IsoIO *ISO::GetIO(std::string filePath)
{
    return new IsoIO(io, filePath, this);
}

IsoIO *ISO::GetIO(GdfxFileEntry *entry)
{
    IsoIO *toReturn = new IsoIO(io, entry, this);
    return toReturn;
}

std::string ISO::GetXGDVersion()
{
    return xgdVersion;
}

UINT64 ISO::GetTotalSectors()
{
    return (io->Length() - gdfxHeaderAddress) / ISO_SECTOR_SIZE;
}

std::wstring ISO::GetTitleName()
{
    if (titleName.size() != 0)
        return titleName;

    // look through all the packages in the root
    for (size_t i = 0; i < root.size(); i++)
    {
        GdfxFileEntry entry = root.at(i);
        switch (entry.magic)
        {
        case CON:
        case LIVE:
        case PIRS:
            IsoIO *io = GetIO(&entry);
            StfsPackage package(io);

            // if the title name isn't null then use that one
            if (package.metaData->titleName.size() != 0)
                return package.metaData->titleName;
        }
    }

    return titleName;
}

DWORD ISO::GetFileMagic(GdfxFileEntry *entry)
{
    std::string lowercasePath = entry->filePath;
    std::transform(lowercasePath.begin(), lowercasePath.end(), lowercasePath.begin(), ::tolower);

    // only read the magic if we're in the root or system update folder
    DWORD magic = 0;
    if (lowercasePath == "" || lowercasePath == "$systemupdate/")
    {
        IsoIO *curIO = GetIO(entry);
        curIO->SetEndian(BigEndian);

        // make sure the file is at least 4 bytes (the size of a DWORD)
        if (curIO->Length() >= 4)
            magic = curIO->ReadDword();
        delete curIO;
    }

    entry->magic = magic;
    return magic;
}

void ISO::ParseISO()
{
    // first we need to find the address of the GDFX header, that depends on the XGD (Xbox game disc) version
    if (ValidGDFXHeader(ISO_XGD1_ADDRESS))
    {
        gdfxHeaderAddress = ISO_XGD1_ADDRESS;
        xgdVersion = "XGD1";
    }
    else if (ValidGDFXHeader(ISO_XGD2_ADDRESS))
    {
        gdfxHeaderAddress = ISO_XGD2_ADDRESS;
        xgdVersion = "XGD2";
    }
    else if (ValidGDFXHeader(ISO_XGD3_ADDRESS))
    {
        gdfxHeaderAddress = ISO_XGD3_ADDRESS;
        xgdVersion = "XGD3";
    }
    else
        throw std::string("ISO: Invalid Xbox 360 ISO.");

    // parse the GDFX header
    io->SetPosition(gdfxHeaderAddress);
    GdfxReadHeader(io, &gdfxHeader);
}

bool ISO::ValidGDFXHeader(UINT64 address)
{
    BYTE gdfx_magic_buffer[GDFX_HEADER_MAGIC_LEN];

    io->SetPosition(address);
    io->ReadBytes(gdfx_magic_buffer, GDFX_HEADER_MAGIC_LEN);

    return memcmp(gdfx_magic_buffer, GDFX_HEADER_MAGIC, GDFX_HEADER_MAGIC_LEN) == 0;
}

void ISO::ReadFileListing(vector<GdfxFileEntry> *entryList, DWORD sector, int size, string path)
{
    // seek to the start of the directory listing
    UINT64 entryAddress = SectorToAddress(sector);
    io->SetPosition(entryAddress);

    GdfxFileEntry current;
    DWORD bytesLeft = size;

    while (bytesLeft != 0)
    {
        current.address = io->GetPosition();
        current.fileIndex = 0;

        // make sure we're not at the end of the file listing
        if (!GdfxReadFileEntry(io, &current) && size != 0)
            break;

        // if it's a non-empty directory, then seek to it and read it's contents
        if (current.attributes & GdfxDirectory && current.size != 0)
        {
            // preserve the current positon
            UINT64 seekAddr = io->GetPosition();

            ReadFileListing(&current.files, current.sector, current.size, path + current.name + "/");

            // reset position to current listing
            io->SetPosition(seekAddr);
        }

        current.filePath = path;
        GetFileMagic(&current);

        entryList->push_back(current);

        // seek to the next entry
        entryAddress += (current.nameLen + 0x11) & 0xFFFFFFFC;
        io->SetPosition(entryAddress);

        // check for end
        DWORD nextBytes = io->ReadDword();
        if (nextBytes == 0xFFFFFFFF)
        {
            if ((size - ISO_SECTOR_SIZE) <= 0)
            {
                // sort the file entries so that directories are first
                std::sort(entryList->begin(), entryList->end(), DirectoryFirstCompareGdfxEntries);
                return;
            }
            else
            {
                size -= ISO_SECTOR_SIZE;
                entryAddress = SectorToAddress(++sector);
                io->SetPosition(entryAddress);
            }
        }

        // calculate the amount of bytes left in the file listing table to process
        bytesLeft -= entryAddress - current.address;

        // back up to the entry
        io->SetPosition(entryAddress);

        // reset the directory
        current.files.clear();
    }

    std::sort(entryList->begin(), entryList->end(), DirectoryFirstCompareGdfxEntries);
}

DWORD ISO::GetTotalCopyIterations(const vector<GdfxFileEntry> *entryList)
{
    DWORD totalCopyIterations = 0;
    for (size_t i = 0; i < entryList->size(); i++)
    {
        GdfxFileEntry entry = entryList->at(i);
        if (entry.attributes & GdfxDirectory)
        {
            totalCopyIterations += GetTotalCopyIterations(&entry.files);
        }
        else
        {
            DWORD sectionsPerFile = entry.size / ISO_COPY_BUFFER_SIZE;
            if (entry.size % ISO_COPY_BUFFER_SIZE != 0)
                sectionsPerFile++;

            totalCopyIterations += sectionsPerFile;
        }
    }

    return totalCopyIterations;
}

void ISO::ExtractAllHelper(std::string outDirectory, std::vector<GdfxFileEntry> *entryList, void (*progress)(void*, DWORD, DWORD), void *arg, DWORD *curProgress, DWORD totalProgress)
{
    // extract all the files in the entry list
    for (size_t i = 0; i < entryList->size(); i++)
    {
        GdfxFileEntry entry = entryList->at(i);

        // if it's a directory then extract all the files inside it
        if (entry.attributes & GdfxDirectory)
        {
            Utils::CreateLocalDirectory(outDirectory + "/" + entry.filePath + entry.name);
            ExtractAllHelper(outDirectory, &entry.files, progress, arg, curProgress, totalProgress);
        }
        else
        {
            std::string outFilePath = outDirectory + "/" + entry.filePath;
            ExtractFileHelper(outFilePath, &entry, progress, arg, curProgress, totalProgress);
        }
    }
}

void ISO::ExtractFileHelper(std::string outDirectory, const GdfxFileEntry *toExtract, void (*progress)(void *, DWORD, DWORD), void *arg, DWORD *curProgress, DWORD totalProgress)
{
    BYTE *copyBuffer = new BYTE[ISO_COPY_BUFFER_SIZE];

    // create the directory incase it doesn't exist
    // this is so this function can work with ExtractAllHelper
    Utils::CreateLocalDirectory(outDirectory);

    // create a new file on the local disk
    std::string outFilePath = outDirectory + "/" + toExtract->name;
    BigFileIO extractedFile(outFilePath, true);

    DWORD totalReads = toExtract->size / ISO_COPY_BUFFER_SIZE;
    if (toExtract->size % ISO_COPY_BUFFER_SIZE != 0)
        totalReads++;

    // seek to the beginning of the file
    UINT64 readAddress = SectorToAddress(toExtract->sector);
    io->SetPosition(readAddress);

    // keep reading into the copy buffer and writing to the local disk until the entire file has been extracted
    for (DWORD x = 0; x < totalReads; x++)
    {
        // if we're at the end of the file then we won't need to read ISO_COPY_BUFFER_SIZE bytes
        DWORD numBytesToCopy = ISO_COPY_BUFFER_SIZE;
        if (x == totalReads - 1 && toExtract->size % ISO_COPY_BUFFER_SIZE != 0)
             numBytesToCopy = toExtract->size % ISO_COPY_BUFFER_SIZE;

        // copy over numBytesToCopy bytes
        io->ReadBytes(copyBuffer, numBytesToCopy);
        extractedFile.WriteBytes(copyBuffer, numBytesToCopy);

        if (curProgress)
            (*curProgress)++;

        if (progress)
            progress(arg, curProgress ? *curProgress : 0, totalProgress);
    }

    extractedFile.Close();
    delete copyBuffer;
}
