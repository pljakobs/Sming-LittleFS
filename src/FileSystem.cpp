/**
 * FileSystem.cpp
 *
 * Created on: 8 April 2021
 *
 * Copyright 2021 mikee47 <mike@sillyhouse.net>
 *
 * This file is part of the Sming-LittleFS Library
 *
 * This library is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, version 3 or later.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this library.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 ****/

#include "include/LittleFS/FileSystem.h"
#include "include/LittleFS/Error.h"
#include <IFS/Util.h>

namespace IFS
{
namespace LittleFS
{
/**
 * @brief LittleFS directory object
 */
struct FileDir {
	lfs_dir_t dir;
};

namespace
{
// littlefs 2.10 is fussy about paths (more 'posix-like')
const char* lfsRootPath = "/";

const char* checkRootPath(const char* path)
{
	return (path && path[0]) ? path : lfsRootPath;
}

void fillStat(Stat& stat, const lfs_info& info)
{
	auto name = info.name;
	if(*name == '/') {
		++name;
	}
	stat.name.copy(name);
	stat.size = info.size;

	stat.attr[FileAttribute::Directory] = (info.type == LFS_TYPE_DIR);
	checkStat(stat);
}

} // namespace

/**
 * @brief map IFS OpenFlags to LFS equivalents
 * @param flags
 * @param sflags OUT the LFS file open flags
 * @retval OpenFlags if non-zero then some flags weren't recognised
 */
OpenFlags mapFileOpenFlags(OpenFlags flags, lfs_open_flags& lfsflags)
{
	uint32_t oflags = 0;

	auto map = [&](OpenFlag flag, lfs_open_flags oflag) {
		if(flags[flag]) {
			oflags |= oflag;
			flags -= flag;
		}
	};

	map(OpenFlag::Append, LFS_O_APPEND);
	map(OpenFlag::Truncate, LFS_O_TRUNC);
	map(OpenFlag::Create, LFS_O_CREAT);
	map(OpenFlag::Read, LFS_O_RDONLY);
	map(OpenFlag::Write, LFS_O_WRONLY);

	flags -= OpenFlag::NoFollow;

	if(flags.any()) {
		debug_w("Unknown OpenFlags: 0x%02X", flags.value());
	}

	lfsflags = lfs_open_flags(oflags);
	return flags;
}

#define CHECK_MOUNTED()                                                                                                \
	if(!mounted) {                                                                                                     \
		return Error::NotMounted;                                                                                      \
	}

#define GET_FD()                                                                                                       \
	CHECK_MOUNTED()                                                                                                    \
	if(file < LFS_HANDLE_MIN || file > LFS_HANDLE_MAX) {                                                               \
		return Error::InvalidHandle;                                                                                   \
	}                                                                                                                  \
	auto& fd = fileDescriptors[file - LFS_HANDLE_MIN];                                                                 \
	if(fd == nullptr) {                                                                                                \
		return Error::FileNotOpen;                                                                                     \
	}

#define CHECK_FILE()                                                                                                   \
	if(fd->isdir()) {                                                                                                  \
		return translateLfsError(LFS_ERR_ISDIR);                                                                       \
	}

#define CHECK_WRITE()                                                                                                  \
	if(!fd->flags[FileDescriptor::Flag::Write]) {                                                                      \
		return Error::ReadOnly;                                                                                        \
	}

FileSystem::~FileSystem()
{
	if(mounted) {
		lfs_unmount(&lfs);
	}
}

int FileSystem::mount()
{
	if(!partition) {
		return Error::NoPartition;
	}

	if(!partition.verify(Storage::Partition::SubType::Data::littlefs)) {
		return Error::BadPartition;
	}

	config.block_count = partition.size() / LFS_BLOCK_SIZE;

	auto res = tryMount();
	if(res < 0) {
		/*
		 * Mount failed, so we either try to repair the system or format it.
		 * For now, just format it.
		 */
		debug_w("[LFS] Mount failed, formatting");
		format();
		res = tryMount();
	}

	return res;
}

int FileSystem::tryMount()
{
	assert(!mounted);
	lfs = lfs_t{};
	auto err = lfs_mount(&lfs, &config);
	if(err < 0) {
		err = translateLfsError(err);
		debug_ifserr(err, "lfs_mount()");
		return err;
	}

	get_attr(lfsRootPath, AttributeTag::ReadAce, rootAcl.readAccess);
	get_attr(lfsRootPath, AttributeTag::WriteAce, rootAcl.writeAccess);

	mounted = true;
	return FS_OK;
}

/*
 * Used by `format` to update partition details
 */
class LfsPartition : public Storage::Partition
{
public:
	void update()
	{
		auto info = const_cast<Info*>(mPart);
		FullType ft(Partition::SubType::Data::littlefs);
		info->type = ft.type;
		info->subtype = ft.subtype;
	}
};

/*
 * Format the file system and leave it mounted in an accessible state.
 */
int FileSystem::format()
{
	auto wasMounted = mounted;
	if(mounted) {
		lfs_unmount(&lfs);
		mounted = false;
	}
	if(!partition) {
		return Error::NoPartition;
	}
	lfs = lfs_t{};
	config.block_count = partition.size() / LFS_BLOCK_SIZE;
	int err = lfs_format(&lfs, &config);
	if(err < 0) {
		err = translateLfsError(err);
		debug_ifserr(err, "format()");
		return err;
	}

	static_cast<LfsPartition&>(partition).update();

	// Re-mount
	return wasMounted ? tryMount() : FS_OK;
}

int FileSystem::check()
{
	return Error::NotImplemented;
}

int FileSystem::getinfo(Info& info)
{
	info.clear();
	info.partition = partition;
	info.type = Type::LittleFS;
	info.maxNameLength = LFS_NAME_MAX;
	info.maxPathLength = UINT16_MAX;
	if(!mounted) {
		return FS_OK;
	}

	info.attr |= Attribute::Mounted;
	info.volumeSize = config.block_count * LFS_BLOCK_SIZE;
	if(info.basicOnly) {
		return FS_OK;
	}

	auto usedBlocks = lfs_fs_size(&lfs);
	if(usedBlocks < 0) {
		return translateLfsError(usedBlocks);
	}
	info.freeSpace = (config.block_count - usedBlocks) * LFS_BLOCK_SIZE;

	return FS_OK;
}

int FileSystem::setProfiler(IProfiler* profiler)
{
	this->profiler = profiler;
	return FS_OK;
}

String FileSystem::getErrorString(int err)
{
	if(Error::isSystem(err)) {
		return lfsErrorToStr(Error::toSystem(err));
	} else {
		return IFS::Error::toString(err);
	}
}

int FileSystem::fgetextents(FileHandle file, Storage::Partition* part, Extent* list, uint16_t extcount)
{
	GET_FD()
	CHECK_FILE()
	auto& f = fd->file;

	if(part) {
		*part = partition;
	}

	int res = lfs_file_seek(&lfs, &f, 0, LFS_SEEK_END);
	if(res < 0) {
		return translateLfsError(res);
	}
	uint32_t fileSize = res;

	uint16_t extIndex{0};
	for(uint32_t offset = 0; offset < fileSize; ++extIndex) {
		res = lfs_file_seek(&lfs, &f, offset, LFS_SEEK_SET);
		if(res < 0) {
			return translateLfsError(res);
		}
		uint8_t c;
		res = lfs_file_read(&lfs, &f, &c, 1);
		if(res < 0) {
			return translateLfsError(res);
		}
		if(f.flags & LFS_F_INLINE) {
			// Inline extents require traversing mdir, not trivial
			return Error::NotSupported;
		}
		auto off = f.off - 1;
		Extent ext{(f.block * LFS_BLOCK_SIZE) + off, std::min(uint32_t(LFS_BLOCK_SIZE - off), fileSize - offset)};
		if(list && extIndex < extcount) {
			list[extIndex] = ext;
		}
		offset += ext.length;
	}

	return extIndex;
}

FileHandle FileSystem::open(const char* path, OpenFlags flags)
{
	CHECK_MOUNTED()
	path = checkRootPath(path);

	// If file is marked read-only, fail write requests
	if(flags[OpenFlag::Write]) {
		FileAttributes attr;
		get_attr(path, AttributeTag::FileAttributes, attr);
		if(attr[FileAttribute::ReadOnly]) {
			return Error::ReadOnly;
		}
	}

	lfs_open_flags oflags;
	if(mapFileOpenFlags(flags, oflags).any()) {
		return FileHandle(Error::NotSupported);
	}

	/*
	 * Allocate a file descriptor
	 */
	int file{Error::OutOfFileDescs};
	for(unsigned i = 0; i < LFS_MAX_FDS; ++i) {
		auto& fd = fileDescriptors[i];
		if(!fd) {
			auto newFd= new FileDescriptor;
			if (!newFd){
				return Error::NoMem;
			}
			fd.reset(newFd);
			file = LFS_HANDLE_MIN + i;
			break;
		}
	}
	if(file < 0) {
		debug_ifserr(file, "open('%s')", path);
		return file;
	}

	auto& fd = fileDescriptors[file - LFS_HANDLE_MIN];
	int err = lfs_file_opencfg(&lfs, &fd->file, path, oflags, &fd->config);
	if(err == LFS_ERR_ISDIR) {
		if(flags - (OpenFlag::Read | OpenFlag::Write | OpenFlag::NoFollow)) {
			return Error::Denied;
		}
		fd->flags[FileDescriptor::Flag::IsDir] = true;
		err = lfs_dir_open(&lfs, &fd->dir, path);
	}
	if(err < 0) {
		err = translateLfsError(err);
		debug_d("open('%s'): %s", path, getErrorString(file).c_str());
		fd.reset();
		return err;
	}

	// Copy name into descriptor
	fd->name = path;

	get_attr(fd->name.c_str(), AttributeTag::ModifiedTime, fd->mtime);

	if(isRootPath(path)) {
		fd->flags += FileDescriptor::Flag::IsRoot;
	}
	fd->flags[FileDescriptor::Flag::Write] = flags[OpenFlag::Write];

	return file;
}

int FileSystem::close(FileHandle file)
{
	GET_FD()

	flushMeta(*fd);

	int res;
	if(fd->isdir()) {
		res = lfs_dir_close(&lfs, &fd->dir);
	} else {
		res = lfs_file_close(&lfs, &fd->file);
	}
	fd.reset();
	return translateLfsError(res);
}

int FileSystem::eof(FileHandle file)
{
	GET_FD()
	CHECK_FILE()

	auto size = lfs_file_size(&lfs, &fd->file);
	if(size < 0) {
		return translateLfsError(size);
	}
	auto pos = lfs_file_tell(&lfs, &fd->file);
	if(pos < 0) {
		return translateLfsError(pos);
	}
	return (pos >= size) ? 1 : 0;
}

file_offset_t FileSystem::tell(FileHandle file)
{
	GET_FD()
	CHECK_FILE()

	int res = lfs_file_tell(&lfs, &fd->file);
	return translateLfsError(res);
}

int FileSystem::ftruncate(FileHandle file, file_size_t new_size)
{
	GET_FD()
	CHECK_FILE()
	CHECK_WRITE()

	int res = lfs_file_truncate(&lfs, &fd->file, new_size);
	return translateLfsError(res);
}

void FileSystem::flushMeta(FileDescriptor& fd)
{
	if(fd.flags[FileDescriptor::Flag::TimeChanged]) {
		fd.flags -= FileDescriptor::Flag::TimeChanged;
		int err = set_attr(fd.name.c_str(), AttributeTag::ModifiedTime, fd.mtime);
		if(err < 0) {
			debug_e("!! set_attr failed %s", Error::toString(err).c_str());
		}
	}
}

int FileSystem::flush(FileHandle file)
{
	GET_FD()
	CHECK_WRITE()

	flushMeta(*fd);

	if(fd->isdir()) {
		return FS_OK;
	}

	int res = lfs_file_sync(&lfs, &fd->file);
	return translateLfsError(res);
}

int FileSystem::read(FileHandle file, void* data, size_t size)
{
	GET_FD()
	CHECK_FILE()

	int res = lfs_file_read(&lfs, &fd->file, data, size);
	if(res < 0) {
		int err = translateLfsError(res);
		debug_ifserr(err, "read()");
		return err;
	}

	return res;
}

int FileSystem::write(FileHandle file, const void* data, size_t size)
{
	GET_FD()
	CHECK_FILE()
	CHECK_WRITE()

	int res = lfs_file_write(&lfs, &fd->file, data, size);
	if(res < 0) {
		return translateLfsError(res);
	}

	fd->touch();
	return res;
}

file_offset_t FileSystem::lseek(FileHandle file, file_offset_t offset, SeekOrigin origin)
{
	GET_FD()
	CHECK_FILE()

	int res = lfs_file_seek(&lfs, &fd->file, offset, int(origin));
	return translateLfsError(res);
}

int FileSystem::stat(const char* path, Stat* stat)
{
	CHECK_MOUNTED()
	path = checkRootPath(path);

	if(stat == nullptr) {
		struct lfs_info info {
		};
		int err = lfs_stat(&lfs, path, &info);
		return translateLfsError(err);
	}

	*stat = Stat{};
	stat->acl = rootAcl;
	StatAttr sa(*stat);
	struct lfs_info info {
	};
	int err = lfs_stata(&lfs, path, &info, sa.attrs, sa.count);
	if(err < 0) {
		return translateLfsError(err);
	}

	stat->fs = this;
	fillStat(*stat, info);
	return FS_OK;
}

int FileSystem::fstat(FileHandle file, Stat* stat)
{
	GET_FD()

	file_size_t size{};
	if(fd->isdir()) {
		if(stat == nullptr) {
			return 0;
		}
	} else {
		size = lfs_file_size(&lfs, &fd->file);
		if(stat == nullptr || size < 0) {
			return translateLfsError(size);
		}
	}

	*stat = Stat{};
	stat->fs = this;
	stat->id = fd->isdir() ? fd->dir.id : fd->file.id;
	stat->name.copy(fd->name.c_str());
	stat->size = size;
	stat->mtime = fd->mtime;
	stat->acl = rootAcl;

	auto callback = [&](AttributeEnum& e) -> bool {
		auto update = [&](void* value) {
			memcpy(value, e.buffer, e.size);
			return true;
		};
		switch(e.tag) {
		case AttributeTag::ReadAce:
			return update(&stat->acl.readAccess);
		case AttributeTag::WriteAce:
			return update(&stat->acl.writeAccess);
		case AttributeTag::Compression:
			return update(&stat->compression);
		case AttributeTag::FileAttributes:
			return update(&stat->attr);
		default:
			return true; // Ignore, continue
		}
	};
	uint8_t buffer[16];
	fenumxattr(file, callback, buffer, sizeof(buffer));
	checkStat(*stat);
	stat->attr[FileAttribute::Directory] = (fd->file.type == LFS_TYPE_DIR);

	return FS_OK;
}

int FileSystem::fsetxattr(FileHandle file, AttributeTag tag, const void* data, size_t size)
{
	GET_FD()
	CHECK_WRITE()

	if(data == nullptr) {
		// Cannot delete standard attributes
		if(tag < AttributeTag::User) {
			return Error::NotSupported;
		}
		return remove_attr(fd->name.c_str(), tag);
	}

	auto attrSize = getAttributeSize(tag);
	if(attrSize != 0 && size != attrSize) {
		return Error::BadParam;
	}

	if(tag == AttributeTag::ModifiedTime) {
		memcpy(&fd->mtime, data, attrSize);
		fd->flags += FileDescriptor::Flag::TimeChanged;
		return FS_OK;
	}

	int res = lfs_setattr(&lfs, fd->name.c_str(), uint8_t(tag), data, size);
	if(res >= 0 && fd->flags[FileDescriptor::Flag::IsRoot]) {
		checkRootAcl(tag, data);
	}

	return translateLfsError(res);
}

void FileSystem::checkRootAcl(AttributeTag tag, const void* value)
{
	if(tag == AttributeTag::ReadAce) {
		rootAcl.readAccess = *static_cast<const UserRole*>(value);
	}
	if(tag == AttributeTag::WriteAce) {
		rootAcl.writeAccess = *static_cast<const UserRole*>(value);
	}
}

int FileSystem::fgetxattr(FileHandle file, AttributeTag tag, void* buffer, size_t size)
{
	GET_FD()

	if(tag == AttributeTag::ModifiedTime) {
		memcpy(buffer, &fd->mtime, std::min(size, sizeof(TimeStamp)));
		return sizeof(TimeStamp);
	}

	return lfs_getattr(&lfs, fd->name.c_str(), uint8_t(tag), buffer, size);
}

int FileSystem::fenumxattr(FileHandle file, AttributeEnumCallback callback, void* buffer, size_t bufsize)
{
	GET_FD()

	auto lfs_callback = [](struct lfs_attr_enum_t* lfs_e, uint8_t type, lfs_size_t attrsize) -> bool {
		AttributeEnum e{lfs_e->buffer, lfs_e->bufsize};
		e.tag = AttributeTag(type);
		e.attrsize = attrsize;
		e.size = std::min(size_t(attrsize), e.bufsize);
		auto& callback = *static_cast<AttributeEnumCallback*>(lfs_e->param);
		return callback(e);
	};
	struct lfs_attr_enum_t lfs_e {
		&callback, buffer, bufsize
	};
	int res = lfs_enumattr(&lfs, fd->name.c_str(), lfs_callback, &lfs_e);
	return translateLfsError(res);
}

int FileSystem::setxattr(const char* path, AttributeTag tag, const void* data, size_t size)
{
	CHECK_MOUNTED()
	path = checkRootPath(path);

	if(data == nullptr) {
		// Cannot delete standard attributes
		if(tag < AttributeTag::User) {
			return Error::NotSupported;
		}
		return remove_attr(path, tag);
	}

	if(tag < AttributeTag::User) {
		if(size < getAttributeSize(tag)) {
			return Error::BadParam;
		}
	} else if(unsigned(tag) > 255) {
		return Error::BadParam;
	}
	int err = lfs_setattr(&lfs, path, uint8_t(tag), data, size);

	if(err >= 0) {
		checkRootAcl(tag, data);
	}

	return translateLfsError(err);
}

int FileSystem::getxattr(const char* path, AttributeTag tag, void* buffer, size_t size)
{
	CHECK_MOUNTED()
	path = checkRootPath(path);

	if(tag < AttributeTag::User) {
		auto attrSize = getAttributeSize(tag);
		if(size < attrSize) {
			return attrSize;
		}
	} else if(unsigned(tag) > 255) {
		return Error::BadParam;
	}

	int res = lfs_getattr(&lfs, path, uint8_t(tag), buffer, size);
	return translateLfsError(res);
}

int FileSystem::opendir(const char* path, DirHandle& dir)
{
	CHECK_MOUNTED()
	path = checkRootPath(path);

	auto d = new FileDir{};
	if(d == nullptr) {
		return Error::NoMem;
	}

	int err = lfs_dir_open(&lfs, &d->dir, path);
	if(err < 0) {
		err = translateLfsError(err);
		delete d;
		return err;
	}
	lfs_dir_seek(&lfs, &d->dir, 2);

	dir = DirHandle(d);
	return FS_OK;
}

int FileSystem::rewinddir(DirHandle dir)
{
	GET_FILEDIR()

	// Skip "." and ".." entries for consistency with other filesystems
	int err = lfs_dir_seek(&lfs, &d->dir, 2);
	return translateLfsError(err);
}

int FileSystem::readdir(DirHandle dir, Stat& stat)
{
	GET_FILEDIR()

	stat = Stat{};
	stat.acl = rootAcl;
	StatAttr sa(stat);
	struct lfs_info info {
	};
	int err = lfs_dir_reada(&lfs, &d->dir, &info, sa.attrs, sa.count);
	if(err == 0) {
		return Error::NoMoreFiles;
	}
	if(err < 0) {
		return translateLfsError(err);
	}

	stat.fs = this;
	stat.id = d->dir.id - 1;
	fillStat(stat, info);
	return FS_OK;
}

int FileSystem::closedir(DirHandle dir)
{
	GET_FILEDIR()

	int err = lfs_dir_close(&lfs, &d->dir);
	delete d;
	return translateLfsError(err);
}

int FileSystem::mkdir(const char* path)
{
	CHECK_MOUNTED()
	if(isRootPath(path)) {
		return Error::BadParam;
	}

	int err = lfs_mkdir(&lfs, path);
	if(err == 0) {
		TimeStamp mtime;
		mtime = fsGetTimeUTC();
		set_attr(path, AttributeTag::ModifiedTime, mtime);
	}
	if(err == LFS_ERR_EXIST) {
		return FS_OK;
	}
	return translateLfsError(err);
}

int FileSystem::rename(const char* oldpath, const char* newpath)
{
	CHECK_MOUNTED()
	if(isRootPath(oldpath) || isRootPath(newpath)) {
		return Error::BadParam;
	}

	int err = lfs_rename(&lfs, oldpath, newpath);
	return translateLfsError(err);
}

int FileSystem::remove(const char* path)
{
	CHECK_MOUNTED()
	if(isRootPath(path)) {
		return Error::BadParam;
	}

	// Check file is not marked read-only
	FileAttributes attr{};
	get_attr(path, AttributeTag::FileAttributes, attr);
	if(attr[FileAttribute::ReadOnly]) {
		return Error::ReadOnly;
	}

	int err = lfs_remove(&lfs, path);
	return translateLfsError(err);
}

int FileSystem::fremove(FileHandle file)
{
	GET_FD()
	CHECK_FILE()

	FileAttributes attr{};
	get_attr(fd->name.c_str(), AttributeTag::FileAttributes, attr);
	if(attr[FileAttribute::ReadOnly]) {
		return Error::ReadOnly;
	}

	/*
	 * TODO: Update littlefs library to support deletion of open file.
	 * Note that we can mark the file descriptor as invalid, but don't release it:
	 * that happens when the user calls `close()`.
	 */
	return Error::NotImplemented;
}

} // namespace LittleFS
} // namespace IFS
