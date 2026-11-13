//----------------------------------------------------
// file: FileSystem.h
//----------------------------------------------------

#ifndef FileSystemH
#define FileSystemH

#define BACKUP_FILE_LEVEL 5

class XRCORE_API EFS_Utils
{
protected:
    bool GetOpenNameInternal(
        pcstr initial, pstr buffer, size_t sz_buf, bool bMulti = false, pcstr offset = 0, int start_flt_ext = -1);

public:
    void _initialize() {}
    void _destroy() {}
    pcstr GenerateName(pcstr base_path, pcstr base_name, pcstr def_ext, pstr out_name, size_t const out_name_size);

    bool GetOpenName(pcstr initial, string_path& buffer, int sz_buf, bool bMulti = false, pcstr offset = 0,
        int start_flt_ext = -1);
    bool GetOpenName(pcstr initial, xr_string& buf, bool bMulti = false, pcstr offset = 0, int start_flt_ext = -1);

    bool GetSaveName(pcstr initial, string_path& buffer, pcstr offset = 0, int start_flt_ext = -1);
    bool GetSaveName(pcstr initial, xr_string& buf, pcstr offset = 0, int start_flt_ext = -1);

    void MarkFile(pcstr fn, bool bDeleteSource);

    xr_string AppendFolderToName(xr_string& tex_name, int depth, BOOL full_name);

    pcstr AppendFolderToName(pstr tex_name, size_t const tex_name_size, int depth, BOOL full_name);
    pcstr AppendFolderToName(pcstr src_name, pstr dest_name, size_t const dest_name_size, int depth, BOOL full_name);

    xr_string ChangeFileExt(pcstr src, pcstr ext);
    xr_string ChangeFileExt(const xr_string& src, pcstr ext);

    static xr_string ExtractFileName(pcstr src);
    static xr_string ExtractFilePath(pcstr src);
    static xr_string ExtractFileExt(pcstr src);
    static xr_string ExcludeBasePath(pcstr full_path, pcstr excl_path);
};
extern XRCORE_API xr_unique_ptr<EFS_Utils> xr_EFS;
#define EFS (*xr_EFS)

// The drive and the folder of `path`, as _splitpath finds them. _splitpath itself gives both
// empty once the folder is longer than _MAX_DIR (256) - every path below a deep fs root -, so the
// call that takes the buffers' sizes is made instead: a folder as long as a string_path holds.
inline void xr_split_folder(pcstr path, string_path& drive, string_path& folder)
{
#if defined(XR_PLATFORM_WINDOWS)
    if (_splitpath_s(path, drive, sizeof drive, folder, sizeof folder, nullptr, 0, nullptr, 0))
        drive[0] = folder[0] = 0;
#else
    _splitpath(path, drive, folder, nullptr, nullptr);
#endif
}

#endif /*_INCDEF_FileSystem_H_*/
