#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/SysCalls/Modules.h"
#include "Emu/SysCalls/CB_FUNC.h"

#include "Emu/FS/VFS.h"
#include "Emu/FS/vfsFile.h"
#include "Emu/FS/vfsDir.h"
#include "Loader/PSF.h"
#include "cellSaveData.h"

#ifdef _WIN32
	#include <windows.h>
	#undef CreateFile
#else
	#include <sys/types.h>
	#include <sys/stat.h>
#endif

extern Module *cellSysutil;

// Auxiliary Classes
class sortSaveDataEntry
{
	u32 sortType;
	u32 sortOrder;
public:
	sortSaveDataEntry(u32 type, u32 order) : sortType(type), sortOrder(order) {}
	bool operator()(const SaveDataEntry& entry1, const SaveDataEntry& entry2) const
	{
		if (sortOrder == CELL_SAVEDATA_SORTORDER_DESCENT)
		{
			if (sortType == CELL_SAVEDATA_SORTTYPE_MODIFIEDTIME)
				return entry1.st_mtime_ >= entry2.st_mtime_;
			if (sortType == CELL_SAVEDATA_SORTTYPE_SUBTITLE)
				return entry1.subtitle >= entry2.subtitle;
		}
		if (sortOrder == CELL_SAVEDATA_SORTORDER_ASCENT)
		{
			if (sortType == CELL_SAVEDATA_SORTTYPE_MODIFIEDTIME)
				return entry1.st_mtime_ < entry2.st_mtime_;
			if (sortType == CELL_SAVEDATA_SORTTYPE_SUBTITLE)
				return entry1.subtitle < entry2.subtitle;
		}
		return true;
	}
};

// Auxiliary Functions
u64 getSaveDataSize(const std::string& dirName)
{
	vfsDir dir(dirName);
	if (!dir.IsOpened())
		return 0;

	u64 totalSize = 0;
	for(const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read()) {
		if (entry->flags & DirEntry_TypeFile) {
			vfsFile file(dirName+"/"+entry->name);
			totalSize += file.GetSize();
		}
	}
	return totalSize;
}

void addSaveDataEntry(std::vector<SaveDataEntry>& saveEntries, const std::string& saveDir)
{
	// PSF parameters
	vfsFile f(saveDir + "/PARAM.SFO");
	PSFLoader psf(f);
	if(!psf.Load(false))
		return;

	// PNG icon
	std::string localPath;
	Emu.GetVFS().GetDevice(saveDir + "/ICON0.PNG", localPath);

	u64 atime = 0;
	u64 mtime = 0;
	u64 ctime = 0;

	cellSysutil->Error("Running _stat in cellSaveData. Please report this to a RPCS3 developer!");

	std::string real_path;
	struct stat buf;

	Emu.GetVFS().GetDevice(f.GetPath(), real_path);

	if (stat(real_path.c_str(), &buf) != 0)
		cellSysutil->Error("stat failed! (%s)", real_path.c_str());
	else
	{
		atime = buf.st_atime;
		mtime = buf.st_mtime;
		ctime = buf.st_ctime;
	}

	SaveDataEntry saveEntry;
	saveEntry.dirName = psf.GetString("SAVEDATA_DIRECTORY");
	saveEntry.listParam = psf.GetString("SAVEDATA_LIST_PARAM");
	saveEntry.title = psf.GetString("TITLE");
	saveEntry.subtitle = psf.GetString("SUB_TITLE");
	saveEntry.details = psf.GetString("DETAIL");
	saveEntry.sizeKB = (u32)(getSaveDataSize(saveDir) / 1024);
	saveEntry.st_atime_ = atime;
	saveEntry.st_mtime_ = mtime;
	saveEntry.st_ctime_ = ctime;
	saveEntry.iconBuf = NULL; // TODO: Here should be the PNG buffer
	saveEntry.iconBufSize = 0; // TODO: Size of the PNG file
	saveEntry.isNew = false;

	saveEntries.push_back(saveEntry);
}

void addNewSaveDataEntry(std::vector<SaveDataEntry>& saveEntries, vm::ptr<CellSaveDataListNewData> newData)
{
	SaveDataEntry saveEntry;
	saveEntry.dirName = newData->dirName.get_ptr();
	saveEntry.title = newData->icon->title.get_ptr();
	saveEntry.subtitle = newData->icon->title.get_ptr();
	saveEntry.iconBuf = newData->icon->iconBuf.get_ptr();
	saveEntry.iconBufSize = newData->icon->iconBufSize;
	saveEntry.isNew = true;
	// TODO: Add information stored in newData->iconPosition. (It's not very relevant)

	saveEntries.push_back(saveEntry);
}

u32 focusSaveDataEntry(const std::vector<SaveDataEntry>& saveEntries, u32 focusPosition)
{
	// TODO: Get the correct index. Right now, this returns the first element of the list.
	return 0;
}

void setSaveDataList(std::vector<SaveDataEntry>& saveEntries, vm::ptr<CellSaveDataDirList> fixedList, u32 fixedListNum)
{
	std::vector<SaveDataEntry>::iterator entry = saveEntries.begin();
	while (entry != saveEntries.end())
	{
		bool found = false;
		for (u32 j = 0; j < fixedListNum; j++)
		{
			if (entry->dirName == (char*)fixedList[j].dirName)
			{
				found = true;
				break;
			}
		}
		if (!found)
			entry = saveEntries.erase(entry);
		else
			entry++;
	}
}

void setSaveDataFixed(std::vector<SaveDataEntry>& saveEntries, vm::ptr<CellSaveDataFixedSet> fixedSet)
{
	std::vector<SaveDataEntry>::iterator entry = saveEntries.begin();
	while (entry != saveEntries.end())
	{
		if (entry->dirName == fixedSet->dirName.get_ptr())
			entry = saveEntries.erase(entry);
		else
			entry++;
	}

	if (saveEntries.size() == 0)
	{
		SaveDataEntry entry;
		entry.dirName = fixedSet->dirName.get_ptr();
		entry.isNew = true;
		saveEntries.push_back(entry);
	}

	if (fixedSet->newIcon)
	{
		saveEntries[0].iconBuf = fixedSet->newIcon->iconBuf.get_ptr();
		saveEntries[0].iconBufSize = fixedSet->newIcon->iconBufSize;
		saveEntries[0].title = fixedSet->newIcon->title.get_ptr();
		saveEntries[0].subtitle = fixedSet->newIcon->title.get_ptr();
	}
}

void getSaveDataStat(SaveDataEntry entry, vm::ptr<CellSaveDataStatGet> statGet)
{
	if (entry.isNew)
		statGet->isNewData = CELL_SAVEDATA_ISNEWDATA_YES;
	else
		statGet->isNewData = CELL_SAVEDATA_ISNEWDATA_NO;

	statGet->bind = 0; // TODO ?
	statGet->sizeKB = entry.sizeKB;
	statGet->hddFreeSizeKB = 40000000; // 40 GB. TODO ?
	statGet->sysSizeKB = 0; // TODO: This is the size of PARAM.SFO + PARAM.PDF
	statGet->dir.st_atime_ = 0; // TODO ?
	statGet->dir.st_mtime_ = 0; // TODO ?
	statGet->dir.st_ctime_ = 0; // TODO ?
	strcpy_trunc(statGet->dir.dirName, entry.dirName);

	statGet->getParam.attribute = 0; // TODO ?
	strcpy_trunc(statGet->getParam.title, entry.title);
	strcpy_trunc(statGet->getParam.subTitle, entry.subtitle);
	strcpy_trunc(statGet->getParam.detail, entry.details);
	strcpy_trunc(statGet->getParam.listParam, entry.listParam);

	statGet->fileNum = 0;
	statGet->fileList.set(0);
	statGet->fileListNum = 0;
	std::string saveDir = "/dev_hdd0/home/00000001/savedata/" + entry.dirName; // TODO: Get the path of the current user
	vfsDir dir(saveDir);
	if (!dir.IsOpened())
		return;

	std::vector<CellSaveDataFileStat> fileEntries;
	for(const DirEntryInfo* dirEntry = dir.Read(); dirEntry; dirEntry = dir.Read()) {
		if (dirEntry->flags & DirEntry_TypeFile) {
			if (dirEntry->name == "PARAM.SFO" || dirEntry->name == "PARAM.PFD")
				continue;

			statGet->fileNum++;
			statGet->fileListNum++;
			CellSaveDataFileStat fileEntry;
			vfsFile file(saveDir + "/" + dirEntry->name);

			if (dirEntry->name == "ICON0.PNG")
				fileEntry.fileType = CELL_SAVEDATA_FILETYPE_CONTENT_ICON0;
			else if (dirEntry->name == "ICON1.PAM")
				fileEntry.fileType = CELL_SAVEDATA_FILETYPE_CONTENT_ICON1;
			else if (dirEntry->name == "PIC1.PNG") 
				fileEntry.fileType = CELL_SAVEDATA_FILETYPE_CONTENT_PIC1;
			else if (dirEntry->name == "SND0.AT3") 
				fileEntry.fileType = CELL_SAVEDATA_FILETYPE_CONTENT_SND0;

			fileEntry.st_size = file.GetSize();
			fileEntry.st_atime_ = 0; // TODO ?
			fileEntry.st_mtime_ = 0; // TODO ?
			fileEntry.st_ctime_ = 0; // TODO ?
			strcpy_trunc(fileEntry.fileName, dirEntry->name);

			fileEntries.push_back(fileEntry);
		}
	}

	statGet->fileList.set((u32)Memory.Alloc(sizeof(CellSaveDataFileStat) * fileEntries.size(), 8));
	for (u32 i = 0; i < fileEntries.size(); i++) {
		CellSaveDataFileStat *dst = &statGet->fileList[i];
		memcpy(dst, &fileEntries[i], sizeof(CellSaveDataFileStat));
	}
}

s32 modifySaveDataFiles(vm::ptr<CellSaveDataFileCallback> funcFile, vm::ptr<CellSaveDataCBResult> result, const std::string& saveDataDir)
{
	vm::var<CellSaveDataFileGet> fileGet;
	vm::var<CellSaveDataFileSet> fileSet;

	if (!Emu.GetVFS().ExistsDir(saveDataDir))
		Emu.GetVFS().CreateDir(saveDataDir);

	fileGet->excSize = 0;
	while (true)
	{
		funcFile(result, fileGet, fileSet);
		if (result->result < 0)	{
			cellSysutil->Error("modifySaveDataFiles: CellSaveDataFileCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
			return CELL_SAVEDATA_ERROR_CBRESULT;
		}
		if (result->result == CELL_SAVEDATA_CBRESULT_OK_LAST || result->result == CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM) {
			break;
		}

		std::string filepath = saveDataDir + '/';
		vfsStream* file = NULL;
		void* buf = fileSet->fileBuf.get_ptr();

		switch ((u32)fileSet->fileType)
		{
		case CELL_SAVEDATA_FILETYPE_SECUREFILE:     filepath += fileSet->fileName.get_ptr(); break;
		case CELL_SAVEDATA_FILETYPE_NORMALFILE:     filepath += fileSet->fileName.get_ptr(); break;
		case CELL_SAVEDATA_FILETYPE_CONTENT_ICON0:  filepath += "ICON0.PNG"; break;
		case CELL_SAVEDATA_FILETYPE_CONTENT_ICON1:  filepath += "ICON1.PAM"; break;
		case CELL_SAVEDATA_FILETYPE_CONTENT_PIC1:   filepath += "PIC1.PNG";  break;
		case CELL_SAVEDATA_FILETYPE_CONTENT_SND0:   filepath += "SND0.AT3";  break;

		default:
			cellSysutil->Error("modifySaveDataFiles: Unknown fileType! Aborting...");
			return CELL_SAVEDATA_ERROR_PARAM;
		}

		switch ((u32)fileSet->fileOperation)
		{
		case CELL_SAVEDATA_FILEOP_READ:
			file = Emu.GetVFS().OpenFile(filepath, vfsRead);
			fileGet->excSize = (u32)file->Read(buf, (u32)std::min(fileSet->fileSize, fileSet->fileBufSize)); // TODO: This may fail for big files because of the dest pointer.
			break;
		
		case CELL_SAVEDATA_FILEOP_WRITE:
			Emu.GetVFS().CreateFile(filepath);
			file = Emu.GetVFS().OpenFile(filepath, vfsWrite);
			fileGet->excSize = (u32)file->Write(buf, (u32)std::min(fileSet->fileSize, fileSet->fileBufSize)); // TODO: This may fail for big files because of the dest pointer.
			break;

		case CELL_SAVEDATA_FILEOP_DELETE:
			Emu.GetVFS().RemoveFile(filepath);
			fileGet->excSize = 0;
			break;

		case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC:
			cellSysutil->Todo("modifySaveDataFiles: CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC");
			break;

		default:
			cellSysutil->Error("modifySaveDataFiles: Unknown fileOperation! Aborting...");
			return CELL_SAVEDATA_ERROR_PARAM;
		}

		if (file && file->IsOpened())
			file->Close();
	}
	return CELL_OK;
}


// Functions
s32 cellSaveDataListSave2(
	u32 version,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataListCallback> funcList,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Warning("cellSaveDataListSave2(version=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcList_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, setList.addr(), setBuf.addr(), funcList.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	vm::var<CellSaveDataCBResult> result;
	vm::var<CellSaveDataListGet> listGet;
	vm::var<CellSaveDataListSet> listSet;
	vm::var<CellSaveDataStatGet> statGet;
	vm::var<CellSaveDataStatSet> statSet;

	std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	vfsDir dir(saveBaseDir);
	if(!dir.IsOpened())
		return CELL_SAVEDATA_ERROR_INTERNAL;

	std::string dirNamePrefix = setList->dirNamePrefix.get_ptr();
	std::vector<SaveDataEntry> saveEntries;

	for(const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	{
		if (entry->flags & DirEntry_TypeDir && entry->name.substr(0,dirNamePrefix.size()) == dirNamePrefix)
		{
			// Count the amount of matches and the amount of listed directories
			listGet->dirListNum++;
			if (listGet->dirListNum > setBuf->dirListMax)
				continue;
			listGet->dirNum++;

			std::string saveDir = saveBaseDir + entry->name;
			addSaveDataEntry(saveEntries, saveDir);
		}
	}

	// Sort the entries and fill the listGet->dirList array
	std::sort(saveEntries.begin(), saveEntries.end(), sortSaveDataEntry(setList->sortType, setList->sortOrder));
	listGet->dirList.set(setBuf->buf.addr());
	auto dirList = listGet->dirList.get_ptr();

	for (u32 i=0; i<saveEntries.size(); i++) {
		strcpy_trunc(dirList[i].dirName, saveEntries[i].dirName);
		strcpy_trunc(dirList[i].listParam, saveEntries[i].listParam);
		*dirList[i].reserved = {};
	}

	funcList(result, listGet, listSet);

	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataListSave2: CellSaveDataListCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}

	setSaveDataList(saveEntries, listSet->fixedList.to_le(), listSet->fixedListNum);
	if (listSet->newData)
		addNewSaveDataEntry(saveEntries, listSet->newData.to_le());
	if (saveEntries.size() == 0) {
		cellSysutil->Error("cellSaveDataListSave2: No save entries found!"); // TODO: Find a better way to handle this error
		return CELL_OK;
	}

	u32 focusIndex = focusSaveDataEntry(saveEntries, listSet->focusPosition);
	// TODO: Display the dialog here
	u32 selectedIndex = focusIndex; // TODO: Until the dialog is implemented, select always the focused entry
	getSaveDataStat(saveEntries[selectedIndex], statGet);
	result->userdata = userdata;

	funcStat(result, statGet, statSet);
	Memory.Free(statGet->fileList.addr());
	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataListLoad2: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}

	/*if (statSet->setParam)
		addNewSaveDataEntry(saveEntries, (u32)listSet->newData.addr()); // TODO: This *is* wrong
	*/

	// Enter the loop where the save files are read/created/deleted.
	s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);

	return ret;
}

s32 cellSaveDataListLoad2(
	u32 version,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataListCallback> funcList,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Warning("cellSaveDataListLoad2(version=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcList_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, setList.addr(), setBuf.addr(), funcList.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	vm::var<CellSaveDataCBResult> result;
	vm::var<CellSaveDataListGet> listGet;
	vm::var<CellSaveDataListSet> listSet;
	vm::var<CellSaveDataStatGet> statGet;
	vm::var<CellSaveDataStatSet> statSet;

	std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	vfsDir dir(saveBaseDir);

	if(!dir.IsOpened())
		return CELL_SAVEDATA_ERROR_INTERNAL;

	std::string dirNamePrefix = setList->dirNamePrefix.get_ptr();
	std::vector<SaveDataEntry> saveEntries;

	for(const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	{
		if (entry->flags & DirEntry_TypeDir && entry->name.substr(0,dirNamePrefix.size()) == dirNamePrefix)
		{
			// Count the amount of matches and the amount of listed directories
			listGet->dirListNum++;
			if (listGet->dirListNum > setBuf->dirListMax)
				continue;
			listGet->dirNum++;

			std::string saveDir = saveBaseDir + entry->name;
			addSaveDataEntry(saveEntries, saveDir);
		}
	}

	// Sort the entries and fill the listGet->dirList array
	std::sort(saveEntries.begin(), saveEntries.end(), sortSaveDataEntry(setList->sortType, setList->sortOrder));
	listGet->dirList.set(setBuf->buf.addr());
	auto dirList = listGet->dirList.get_ptr();

	for (u32 i=0; i<saveEntries.size(); i++) {
		strcpy_trunc(dirList[i].dirName, saveEntries[i].dirName);
		strcpy_trunc(dirList[i].listParam, saveEntries[i].listParam);
		*dirList[i].reserved = {};
	}

	funcList(result, listGet, listSet);

	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataListLoad2: CellSaveDataListCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}

	setSaveDataList(saveEntries, listSet->fixedList.to_le(), listSet->fixedListNum);
	if (listSet->newData)
		addNewSaveDataEntry(saveEntries, listSet->newData.to_le());
	if (saveEntries.size() == 0) {
		cellSysutil->Error("cellSaveDataListLoad2: No save entries found!"); // TODO: Find a better way to handle this error
		return CELL_OK;
	}

	u32 focusIndex = focusSaveDataEntry(saveEntries, listSet->focusPosition);
	// TODO: Display the dialog here
	u32 selectedIndex = focusIndex; // TODO: Until the dialog is implemented, select always the focused entry
	getSaveDataStat(saveEntries[selectedIndex], statGet);
	result->userdata = userdata;

	funcStat(result, statGet, statSet);
	Memory.Free(statGet->fileList.addr());
	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataListLoad2: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}

	/*if (statSet->setParam)
		// TODO: Write PARAM.SFO file
	*/

	// Enter the loop where the save files are read/created/deleted.
	s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);

	return ret;
}

s32 cellSaveDataFixedSave2(
	u32 version,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Warning("cellSaveDataFixedSave2(version=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	vm::var<CellSaveDataCBResult> result;
	vm::var<CellSaveDataListGet> listGet;
	vm::var<CellSaveDataFixedSet> fixedSet;
	vm::var<CellSaveDataStatGet> statGet;
	vm::var<CellSaveDataStatSet> statSet;

	std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	vfsDir dir(saveBaseDir);
	if (!dir.IsOpened())
		return CELL_SAVEDATA_ERROR_INTERNAL;

	std::string dirNamePrefix = setList->dirNamePrefix.get_ptr();
	std::vector<SaveDataEntry> saveEntries;
	for (const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	{
		if (entry->flags & DirEntry_TypeDir && entry->name.substr(0, dirNamePrefix.size()) == dirNamePrefix)
		{
			// Count the amount of matches and the amount of listed directories
			listGet->dirListNum++;
			if (listGet->dirListNum > setBuf->dirListMax)
				continue;
			listGet->dirNum++;

			std::string saveDir = saveBaseDir + entry->name;
			addSaveDataEntry(saveEntries, saveDir);
		}
	}

	// Sort the entries and fill the listGet->dirList array
	std::sort(saveEntries.begin(), saveEntries.end(), sortSaveDataEntry(setList->sortType, setList->sortOrder));
	listGet->dirList.set(setBuf->buf.addr());
	auto dirList = listGet->dirList.get_ptr();
	for (u32 i = 0; i<saveEntries.size(); i++) {
		strcpy_trunc(dirList[i].dirName, saveEntries[i].dirName);
		strcpy_trunc(dirList[i].listParam, saveEntries[i].listParam);
		*dirList[i].reserved = {};
	}
	funcFixed(result, listGet, fixedSet);
	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataFixedSave2: CellSaveDataFixedCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}
	setSaveDataFixed(saveEntries, fixedSet);
	getSaveDataStat(saveEntries[0], statGet); // There should be only one element in this list
	// TODO: Display the Yes|No dialog here
	result->userdata = userdata;

	funcStat(result, statGet, statSet);
	Memory.Free(statGet->fileList.addr());
	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataFixedSave2: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}
	/*if (statSet->setParam)
		// TODO: Write PARAM.SFO file
	*/

	// Enter the loop where the save files are read/created/deleted.
	s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);

	return ret;
}

s32 cellSaveDataFixedLoad2(
	u32 version,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Warning("cellSaveDataFixedLoad2(version=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	vm::var<CellSaveDataCBResult> result;
	vm::var<CellSaveDataListGet> listGet;
	vm::var<CellSaveDataFixedSet> fixedSet;
	vm::var<CellSaveDataStatGet> statGet;
	vm::var<CellSaveDataStatSet> statSet;

	std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	vfsDir dir(saveBaseDir);
	if (!dir.IsOpened())
		return CELL_SAVEDATA_ERROR_INTERNAL;

	std::string dirNamePrefix = setList->dirNamePrefix.get_ptr();
	std::vector<SaveDataEntry> saveEntries;
	for (const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	{
		if (entry->flags & DirEntry_TypeDir && entry->name.substr(0, dirNamePrefix.size()) == dirNamePrefix)
		{
			// Count the amount of matches and the amount of listed directories
			listGet->dirListNum++;
			if (listGet->dirListNum > setBuf->dirListMax)
				continue;
			listGet->dirNum++;

			std::string saveDir = saveBaseDir + entry->name;
			addSaveDataEntry(saveEntries, saveDir);
		}
	}

	// Sort the entries and fill the listGet->dirList array
	std::sort(saveEntries.begin(), saveEntries.end(), sortSaveDataEntry(setList->sortType, setList->sortOrder));
	listGet->dirList.set(setBuf->buf.addr());
	auto dirList = listGet->dirList.get_ptr();
	for (u32 i = 0; i<saveEntries.size(); i++) {
		strcpy_trunc(dirList[i].dirName, saveEntries[i].dirName);
		strcpy_trunc(dirList[i].listParam, saveEntries[i].listParam);
		*dirList[i].reserved = {};
	}
	funcFixed(result, listGet, fixedSet);
	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataFixedLoad2: CellSaveDataFixedCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}
	setSaveDataFixed(saveEntries, fixedSet);
	getSaveDataStat(saveEntries[0], statGet); // There should be only one element in this list
	// TODO: Display the Yes|No dialog here
	result->userdata = userdata;

	funcStat(result, statGet, statSet);
	Memory.Free(statGet->fileList.addr());
	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataFixedLoad2: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}
	/*if (statSet->setParam)
		// TODO: Write PARAM.SFO file
	*/

	// Enter the loop where the save files are read/created/deleted.
	s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);

	return ret;
}

s32 cellSaveDataAutoSave2(
	u32 version,
	vm::ptr<const char> dirName,
	u32 errDialog,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Warning("cellSaveDataAutoSave2(version=%d, dirName_addr=0x%x, errDialog=%d, setBuf_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, dirName.addr(), errDialog, setBuf.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	vm::var<CellSaveDataCBResult> result;
	vm::var<CellSaveDataStatGet> statGet;
	vm::var<CellSaveDataStatSet> statSet;

	std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	vfsDir dir(saveBaseDir);
	if (!dir.IsOpened())
		return CELL_SAVEDATA_ERROR_INTERNAL;

	std::string dirN = dirName.get_ptr();
	std::vector<SaveDataEntry> saveEntries;
	for (const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	{
		if (entry->flags & DirEntry_TypeDir && entry->name == dirN) {
			addSaveDataEntry(saveEntries, saveBaseDir + dirN);
		}
	}

	// The target entry does not exist
	if (saveEntries.size() == 0) {
		SaveDataEntry entry;
		entry.dirName = dirN;
		entry.sizeKB = 0;
		entry.isNew = true;
		saveEntries.push_back(entry);
	}

	getSaveDataStat(saveEntries[0], statGet); // There should be only one element in this list
	result->userdata = userdata;
	funcStat(result, statGet, statSet);

	if (statGet->fileList)
		Memory.Free(statGet->fileList.addr());

	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataAutoSave2: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}
	/*if (statSet->setParam)
		// TODO: Write PARAM.SFO file
	*/

	// Enter the loop where the save files are read/created/deleted.
	s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);

	return CELL_OK;
}

s32 cellSaveDataAutoLoad2(
	u32 version,
	vm::ptr<const char> dirName,
	u32 errDialog,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Warning("cellSaveDataAutoLoad2(version=%d, dirName_addr=0x%x, errDialog=%d, setBuf_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, dirName.addr(), errDialog, setBuf.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	vm::var<CellSaveDataCBResult> result;
	vm::var<CellSaveDataStatGet> statGet;
	vm::var<CellSaveDataStatSet> statSet;

	std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	vfsDir dir(saveBaseDir);
	if (!dir.IsOpened())
		return CELL_SAVEDATA_ERROR_INTERNAL;

	std::string dirN = dirName.get_ptr();
	std::vector<SaveDataEntry> saveEntries;
	for (const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	{
		if (entry->flags & DirEntry_TypeDir && entry->name == dirN) {
			addSaveDataEntry(saveEntries, saveBaseDir + dirN);
		}
	}

	// The target entry does not exist
	if (saveEntries.size() == 0) {
		cellSysutil->Error("cellSaveDataAutoLoad2: Couldn't find save entry (%s)", dirN.c_str());
		return CELL_OK; // TODO: Can anyone check the actual behaviour of a PS3 when saves are not found?
	}

	getSaveDataStat(saveEntries[0], statGet); // There should be only one element in this list
	result->userdata = userdata;
	funcStat(result, statGet, statSet);

	Memory.Free(statGet->fileList.addr());
	if (result->result < 0)	{
		cellSysutil->Error("cellSaveDataAutoLoad2: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
		return CELL_SAVEDATA_ERROR_CBRESULT;
	}
	/*if (statSet->setParam)
		// TODO: Write PARAM.SFO file
	*/

	// Enter the loop where the save files are read/created/deleted.
	s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);

	return CELL_OK;
}

s32 cellSaveDataListAutoSave(
	u32 version,
	u32 errDialog,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataListAutoSave(version=%d, errDialog=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, errDialog, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	//vm::var<CellSaveDataCBResult> result;
	//vm::var<CellSaveDataListGet> listGet;
	//vm::var<CellSaveDataFixedSet> fixedSet;
	//vm::var<CellSaveDataStatGet> statGet;
	//vm::var<CellSaveDataStatSet> statSet;

	//std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	//vfsDir dir(saveBaseDir);

	//if (!dir.IsOpened())
	//	return CELL_SAVEDATA_ERROR_INTERNAL;

	//std::string dirNamePrefix = setList->dirNamePrefix.get_ptr();
	//std::vector<SaveDataEntry> saveEntries;

	//for (const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	//{
	//	if (entry->flags & DirEntry_TypeDir && entry->name.substr(0, dirNamePrefix.size()) == dirNamePrefix)
	//	{
	//		// Count the amount of matches and the amount of listed directories
	//		listGet->dirListNum++;
	//		if (listGet->dirListNum > setBuf->dirListMax)
	//			continue;
	//		listGet->dirNum++;

	//		std::string saveDir = saveBaseDir + entry->name;
	//		addSaveDataEntry(saveEntries, saveDir);
	//	}
	//}

	//// Sort the entries and fill the listGet->dirList array
	//std::sort(saveEntries.begin(), saveEntries.end(), sortSaveDataEntry(setList->sortType, setList->sortOrder));
	//listGet->dirList = vm::bptr<CellSaveDataDirList>::make(setBuf->buf.addr());
	//auto dirList = vm::get_ptr<CellSaveDataDirList>(listGet->dirList.addr());

	//for (u32 i = 0; i<saveEntries.size(); i++) {
	//	strcpy_trunc(dirList[i].dirName, saveEntries[i].dirName.c_str());
	//	strcpy_trunc(dirList[i].listParam, saveEntries[i].listParam.c_str());
	//	*dirList[i].reserved = {};
	//}

	//funcFixed(result, listGet, fixedSet);

	//if (result->result < 0)	{
	//	cellSysutil->Error("cellSaveDataListAutoSave: CellSaveDataListCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
	//	return CELL_SAVEDATA_ERROR_CBRESULT;
	//}

	//setSaveDataFixed(saveEntries, fixedSet);
	//getSaveDataStat(saveEntries[0], statGet); // There should be only one element in this list
	//// TODO: Display the Yes|No dialog here
	//result->userdata = userdata;

	//funcStat(result, statGet, statSet);
	//Memory.Free(statGet->fileList.addr());
	//if (result->result < 0)	{
	//	cellSysutil->Error("cellSaveDataListAutoSave: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
	//	return CELL_SAVEDATA_ERROR_CBRESULT;
	//}

	///*if (statSet->setParam)
	//// TODO: Write PARAM.SFO file
	//*/

	//// Enter the loop where the save files are read/created/deleted.
	//s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);
	return CELL_OK;
}

s32 cellSaveDataListAutoLoad(
	u32 version,
	u32 errDialog,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataListAutoLoad(version=%d, errDialog=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, errDialog, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	//vm::var<CellSaveDataCBResult> result;
	//vm::var<CellSaveDataListGet> listGet;
	//vm::var<CellSaveDataFixedSet> fixedSet;
	//vm::var<CellSaveDataStatGet> statGet;
	//vm::var<CellSaveDataStatSet> statSet;

	//std::string saveBaseDir = "/dev_hdd0/home/00000001/savedata/"; // TODO: Get the path of the current user
	//vfsDir dir(saveBaseDir);

	//if (!dir.IsOpened())
	//	return CELL_SAVEDATA_ERROR_INTERNAL;

	//std::string dirNamePrefix = setList->dirNamePrefix.get_ptr();
	//std::vector<SaveDataEntry> saveEntries;

	//for (const DirEntryInfo* entry = dir.Read(); entry; entry = dir.Read())
	//{
	//	if (entry->flags & DirEntry_TypeDir && entry->name.substr(0, dirNamePrefix.size()) == dirNamePrefix)
	//	{
	//		// Count the amount of matches and the amount of listed directories
	//		listGet->dirListNum++;
	//		if (listGet->dirListNum > setBuf->dirListMax)
	//			continue;
	//		listGet->dirNum++;

	//		std::string saveDir = saveBaseDir + entry->name;
	//		addSaveDataEntry(saveEntries, saveDir);
	//	}
	//}

	//// Sort the entries and fill the listGet->dirList array
	//std::sort(saveEntries.begin(), saveEntries.end(), sortSaveDataEntry(setList->sortType, setList->sortOrder));
	//listGet->dirList = vm::bptr<CellSaveDataDirList>::make(setBuf->buf.addr());
	//auto dirList = vm::get_ptr<CellSaveDataDirList>(listGet->dirList.addr());

	//for (u32 i = 0; i<saveEntries.size(); i++) {
	//	strcpy_trunc(dirList[i].dirName, saveEntries[i].dirName);
	//	strcpy_trunc(dirList[i].listParam, saveEntries[i].listParam);
	//	*dirList[i].reserved = {};
	//}

	//funcFixed(result, listGet, fixedSet);

	//if (result->result < 0)	{
	//	cellSysutil->Error("cellSaveDataListAutoLoad: CellSaveDataListCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
	//	return CELL_SAVEDATA_ERROR_CBRESULT;
	//}

	//setSaveDataFixed(saveEntries, fixedSet);
	//getSaveDataStat(saveEntries[0], statGet); // There should be only one element in this list
	//// TODO: Display the Yes|No dialog here
	//result->userdata = userdata;

	//funcStat(result, statGet, statSet);
	//Memory.Free(statGet->fileList.addr());
	//if (result->result < 0)	{
	//	cellSysutil->Error("cellSaveDataFixedLoad2: CellSaveDataStatCallback failed."); // TODO: Once we verify that the entire SysCall is working, delete this debug error message.
	//	return CELL_SAVEDATA_ERROR_CBRESULT;
	//}
	///*if (statSet->setParam)
	//// TODO: Write PARAM.SFO file
	//*/

	//// Enter the loop where the save files are read/created/deleted.
	//s32 ret = modifySaveDataFiles(funcFile, result, saveBaseDir + (char*)statGet->dir.dirName);
	return CELL_OK;
}

s32 cellSaveDataDelete2(u32 container)
{	 
	cellSysutil->Todo("cellSaveDataDelete2(container=%d)", container);

	return CELL_SAVEDATA_RET_CANCEL;
}

s32 cellSaveDataFixedDelete(
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataFixedDelete(setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcDone_addr=0x%x, container=%d, userdata_addr=0x%x)",
		setList.addr(), setBuf.addr(), funcFixed.addr(), funcDone.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserListSave(
	u32 version,
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataListCallback> funcList,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserListSave(version=%d, userId=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcList_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, setList.addr(), setBuf.addr(), funcList.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserListLoad(
	u32 version,
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataListCallback> funcList,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserListLoad(version=%d, userId=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcList_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, setList.addr(), setBuf.addr(), funcList.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserFixedSave(
	u32 version,
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserFixedSave(version=%d, userId=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserFixedLoad(
	u32 version,
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserFixedLoad(version=%d, userId=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserAutoSave(
	u32 version,
	u32 userId,
	vm::ptr<const char> dirName,
	u32 errDialog,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserAutoSave(version=%d, userId=%d, dirName_addr=0x%x, errDialog=%d, setBuf_addr=0x%x, funcStat_addr=0x%x, funcFile=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, dirName.addr(), errDialog, setBuf.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserAutoLoad(
	u32 version,
	u32 userId,
	vm::ptr<const char> dirName,
	u32 errDialog,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserAutoLoad(version=%d, userId=%d, dirName_addr=0x%x, errDialog=%d, setBuf_addr=0x%x, funcStat_addr=0x%x, funcFile=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, dirName.addr(), errDialog, setBuf.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserListAutoSave(
	u32 version,
	u32 userId,
	u32 errDialog,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserListAutoSave(version=%d, userId=%d, errDialog=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, errDialog, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserListAutoLoad(
	u32 version,
	u32 userId,
	u32 errDialog,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataStatCallback> funcStat,
	vm::ptr<CellSaveDataFileCallback> funcFile,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserListAutoLoad(version=%d, userId=%d, errDialog=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcStat_addr=0x%x, funcFile_addr=0x%x, container=%d, userdata_addr=0x%x)",
		version, userId, errDialog, setList.addr(), setBuf.addr(), funcFixed.addr(), funcStat.addr(), funcFile.addr(), container, userdata.addr());

	return CELL_OK;
}

s32 cellSaveDataUserFixedDelete(
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataFixedCallback> funcFixed,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	cellSysutil->Todo("cellSaveDataUserFixedDelete(userId=%d, setList_addr=0x%x, setBuf_addr=0x%x, funcFixed_addr=0x%x, funcDone_addr=0x%x, container=%d, userdata_addr=0x%x)",
		userId, setList.addr(), setBuf.addr(), funcFixed.addr(), funcDone.addr(), container, userdata.addr());

	return CELL_OK;
}

void cellSaveDataEnableOverlay(s32 enable)
{
	cellSysutil->Todo("cellSaveDataEnableOverlay(enable=%d)", enable);

	return;
}


// Functions (Extensions) 
s32 cellSaveDataListDelete(
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataListCallback> funcList,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataListImport(
	vm::ptr<CellSaveDataSetList> setList,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataListExport(
	vm::ptr<CellSaveDataSetList> setList,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataFixedImport(
	vm::ptr<const char> dirName,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataFixedExport(
	vm::ptr<const char> dirName,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataGetListItem(
	vm::ptr<const char> dirName,
	vm::ptr<CellSaveDataDirStat> dir,
	vm::ptr<CellSaveDataSystemFileParam> sysFileParam,
	vm::ptr<u32> bind,
	vm::ptr<u32> sizeKB)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataUserListDelete(
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	vm::ptr<CellSaveDataSetBuf> setBuf,
	vm::ptr<CellSaveDataListCallback> funcList,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataUserListImport(
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataUserListExport(
	u32 userId,
	vm::ptr<CellSaveDataSetList> setList,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataUserFixedImport(
	u32 userId,
	vm::ptr<const char> dirName,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataUserFixedExport(
	u32 userId,
	vm::ptr<const char> dirName,
	u32 maxSizeKB,
	vm::ptr<CellSaveDataDoneCallback> funcDone,
	u32 container,
	vm::ptr<void> userdata)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

s32 cellSaveDataUserGetListItem(
	u32 userId,
	vm::ptr<const char> dirName,
	vm::ptr<CellSaveDataDirStat> dir,
	vm::ptr<CellSaveDataSystemFileParam> sysFileParam,
	vm::ptr<u32> bind,
	vm::ptr<u32> sizeKB)
{
	UNIMPLEMENTED_FUNC(cellSysutil);

	return CELL_OK;
}

void cellSysutil_SaveData_init()
{
	// libsysutil functions:

	REG_FUNC(cellSysutil, cellSaveDataEnableOverlay);

	REG_FUNC(cellSysutil, cellSaveDataDelete2);
	//REG_FUNC(cellSysutil, cellSaveDataDelete);
	REG_FUNC(cellSysutil, cellSaveDataUserFixedDelete);
	REG_FUNC(cellSysutil, cellSaveDataFixedDelete);

	REG_FUNC(cellSysutil, cellSaveDataUserFixedLoad);
	REG_FUNC(cellSysutil, cellSaveDataUserFixedSave);
	REG_FUNC(cellSysutil, cellSaveDataFixedLoad2);
	REG_FUNC(cellSysutil, cellSaveDataFixedSave2);
	//REG_FUNC(cellSysutil, cellSaveDataFixedLoad);
	//REG_FUNC(cellSysutil, cellSaveDataFixedSave);

	REG_FUNC(cellSysutil, cellSaveDataUserListLoad);
	REG_FUNC(cellSysutil, cellSaveDataUserListSave);
	REG_FUNC(cellSysutil, cellSaveDataListLoad2);
	REG_FUNC(cellSysutil, cellSaveDataListSave2);
	//REG_FUNC(cellSysutil, cellSaveDataListLoad);
	//REG_FUNC(cellSysutil, cellSaveDataListSave);

	REG_FUNC(cellSysutil, cellSaveDataUserListAutoLoad);
	REG_FUNC(cellSysutil, cellSaveDataUserListAutoSave);
	REG_FUNC(cellSysutil, cellSaveDataListAutoLoad);
	REG_FUNC(cellSysutil, cellSaveDataListAutoSave);

	REG_FUNC(cellSysutil, cellSaveDataUserAutoLoad);
	REG_FUNC(cellSysutil, cellSaveDataUserAutoSave);
	REG_FUNC(cellSysutil, cellSaveDataAutoLoad2);
	REG_FUNC(cellSysutil, cellSaveDataAutoSave2);
	//REG_FUNC(cellSysutil, cellSaveDataAutoLoad);
	//REG_FUNC(cellSysutil, cellSaveDataAutoSave);

	// libsysutil_savedata functions:
	REG_FUNC(cellSysutil, cellSaveDataUserGetListItem);
	REG_FUNC(cellSysutil, cellSaveDataGetListItem);
	REG_FUNC(cellSysutil, cellSaveDataUserListDelete);
	REG_FUNC(cellSysutil, cellSaveDataListDelete);
	REG_FUNC(cellSysutil, cellSaveDataUserFixedExport);
	REG_FUNC(cellSysutil, cellSaveDataUserFixedImport);
	REG_FUNC(cellSysutil, cellSaveDataUserListExport);
	REG_FUNC(cellSysutil, cellSaveDataUserListImport);
	REG_FUNC(cellSysutil, cellSaveDataFixedExport);
	REG_FUNC(cellSysutil, cellSaveDataFixedImport);
	REG_FUNC(cellSysutil, cellSaveDataListExport);
	REG_FUNC(cellSysutil, cellSaveDataListImport);

	// libsysutil_savedata_psp functions:
}
