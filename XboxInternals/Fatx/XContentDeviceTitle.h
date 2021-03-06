#ifndef XCONTENTDEVICETITLE_H
#define XCONTENTDEVICETITLE_H

#include <iostream>
#include "Stfs/StfsPackage.h"
#include "XContentDeviceItem.h"

#include "XboxInternals_global.h"

class XBOXINTERNALSSHARED_EXPORT XContentDeviceTitle : public XContentDeviceItem
{
public:
    XContentDeviceTitle();
    XContentDeviceTitle(std::string pathOnDevice, std::string rawName);

    std::wstring GetName();
    BYTE* GetThumbnail();
    DWORD GetThumbnailSize();
    DWORD GetTitleID();
    BYTE* GetProfileID();

    vector<XContentDeviceItem> titleSaves;

private:
    std::string pathOnDevice;
};

#endif // XCONTENTDEVICETITLE_H
