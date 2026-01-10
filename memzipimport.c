#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "structmember.h"
#include "osdefs.h"
#include "marshal.h"
#include <time.h>

#ifndef MAXPATHLEN
#define MAXPATHLEN 4096
#endif

#define IS_SOURCE   0x0
#define IS_BYTECODE 0x1
#define IS_PACKAGE  0x2

struct st_zip_searchorder {
    char suffix[14];
    int type;
};

/* zip_searchorder defines how we search for a module in the Zip
   archive: we first search for a package __init__, then for
   non-package .pyc, .pyo and .py entries. The .pyc and .pyo entries
   are swapped by initzipimport() if we run in optimized mode. Also,
   '/' is replaced by SEP there. */
static struct st_zip_searchorder zip_searchorder[] = {
    {"/__init__.pyc", IS_PACKAGE | IS_BYTECODE},
    {"/__init__.pyo", IS_PACKAGE | IS_BYTECODE},
    {"/__init__.py", IS_PACKAGE | IS_SOURCE},
    {".pyc", IS_BYTECODE},
    {".pyo", IS_BYTECODE},
    {".py", IS_SOURCE},
    {"", 0}
};

/* zipimporter object definition and support */

typedef struct _zipimporter ZipImporter;

struct _zipimporter {
    PyObject_HEAD
    PyObject *archive;  /* pathname of the Zip archive (Unicode) */
    PyObject *prefix;   /* file prefix: "a/sub/directory/" (Unicode) */
    PyObject *files;    /* dict with file info {path: toc_entry} */
    PyObject *zip_content; /* bytearray or bytes */
};

static PyObject *ZipImportError;
static PyObject *zip_directory_cache = NULL;

/* forward decls */
static PyObject *read_directory(const char *archive, PyObject *zip_content);
static PyObject *get_data(const char *archive, PyObject *zip_content, PyObject *toc_entry);
static PyObject *get_module_code(ZipImporter *self, const char *fullname,
                                 int *p_ispackage, PyObject **p_modpath);


#define ZipImporter_Check(op) PyObject_TypeCheck(op, &ZipImporter_Type)


/* zipimporter.__init__
   Split the "subdirectory" from the Zip archive path, lookup a matching
   entry in sys.path_importer_cache, fetch the file directory from there
   if found, or else read it from the archive. */
static int
zipimporter_init(ZipImporter *self, PyObject *args, PyObject *kwds)
{
    const char *path;
    char buf[MAXPATHLEN+2];
    char *p;
    char *prefix_str = NULL;
    size_t len;

    PyObject *mem_dict;
    PyObject *path_obj = NULL;

    if (!_PyArg_NoKeywords("zipimporter()", kwds))
        return -1;

    /* In Python 3, 's' argument format converts string to char* using utf-8 usually,
       or filesystem encoding? 's' uses default encoding.
       We use 's' assuming the path is passable as string. */
    if (!PyArg_ParseTuple(args, "s:zipimporter",
                          &path))
        return -1;

    len = strlen(path);
    if (len == 0) {
        PyErr_SetString(ZipImportError, "archive path is empty");
        return -1;
    }
    if (len >= MAXPATHLEN) {
        PyErr_SetString(ZipImportError,
                        "archive path too long");
        return -1;
    }
    strcpy(buf, path);

    /* Split path into archive and prefix */
    /* Logic: find separator. But here we assume the user passed a path
       that *is* the key in memzip, or a subdirectory of it?
       The original code assumes 'buf' is potentially modified to find the root zip. */

    path_obj = PyUnicode_FromString(buf); /* Temporarily wrap buf */
    if (!path_obj) return -1;

    /* We need to search recursively? The original code does a loop stripping components. */
    /* For memzip, the key in sys.memzip is the "archive path". */

    mem_dict = PySys_GetObject("memzip");
    if (mem_dict == NULL || PyDict_Check(mem_dict) == 0) {
        PyErr_SetString(ZipImportError,
                        "memzip not exists");
        Py_DECREF(path_obj);
        return -1;
    }

    /* In Py3, dictionary keys are unicode. We use PyDict_GetItem (with object)
       instead of GetItemString (which converts from utf8). */

    /* Search loop */
    char *search_path = buf;
    char *prefix_ptr = NULL;

    for (;;) {
        PyObject *search_obj = PyUnicode_FromString(search_path);
        if (!search_obj) {
            Py_DECREF(path_obj);
            return -1;
        }

        self->zip_content = PyDict_GetItem(mem_dict, search_obj);
        Py_DECREF(search_obj);

        if (self->zip_content != NULL) {
            break;
        }

        /* back up one path element */
        p = strrchr(search_path, SEP);
        if (p == NULL)
            break;

        if (prefix_ptr != NULL)
            *prefix_ptr = SEP; /* restore previous separator */

        *p = '\0';
        prefix_ptr = p;
    }

    if (self->zip_content == NULL || (PyByteArray_Check(self->zip_content) == 0 && PyBytes_Check(self->zip_content) == 0)) {
        PyErr_SetString(ZipImportError,
                        "zip content not found or invalid type");
        Py_XDECREF(path_obj);
        return -1;
    }
    Py_INCREF(self->zip_content);

    /* Calculate prefix */
    /* search_path is now the archive path */
    /* original path was 'path' */
    /* prefix is the part of 'path' after 'search_path' */

    if (prefix_ptr != NULL) {
         *prefix_ptr = SEP; /* Restore the full path in buf */
         prefix_str = prefix_ptr + 1;
    } else {
        prefix_str = "";
    }

    if (*prefix_str) {
        len = strlen(prefix_str);
        /* Ensure trailing SEP if not empty */
        /* The buffer 'buf' should have space because we checked MAXPATHLEN */
        if (prefix_str[len-1] != SEP) {
            /* This modifies buf, but prefix_str points into buf */
             /* Be careful with overlapping if we were copying, but we are appending */
             /* Actually prefix_str is inside buf. */
             size_t prefix_offset = prefix_str - buf;
             buf[prefix_offset + len] = SEP;
             buf[prefix_offset + len + 1] = '\0';
        }
    }

    /* Archive path is 'search_path' which is effectively 'buf' up to where we stopped */
    /* But we restored buf. */
    /* Let's reconstruct. */
    /* The loop modified buf, found the entry, then we restored it. */
    /* So 'buf' holds the full path. */
    /* We need 'archive' (path to zip) and 'prefix' (internal path). */

    /* Re-find the split point based on logic above */
    /* If prefix_ptr was NULL, archive is full path, prefix empty. */
    /* If prefix_ptr was set, it pointed to the separator before prefix. */

    if (prefix_ptr) {
        *prefix_ptr = '\0';
        self->archive = PyUnicode_FromString(buf);
        *prefix_ptr = SEP;
        self->prefix = PyUnicode_FromString(prefix_ptr + 1);
    } else {
        self->archive = PyUnicode_FromString(buf);
        self->prefix = PyUnicode_FromString("");
    }

    if (!self->archive || !self->prefix) {
        Py_XDECREF(path_obj);
        return -1;
    }

    /* Cache handling */
    PyObject *files;
    /* zip_directory_cache keys are archive paths (unicode) */
    files = PyDict_GetItem(zip_directory_cache, self->archive);
    if (files == NULL) {
        /* Need to pass C string of archive for error msgs etc, but read_directory needs data */
        /* We can use the unicode object to get utf8 chars */
        PyObject *archive_bytes = PyUnicode_AsUTF8String(self->archive);
        if (!archive_bytes) return -1;

        files = read_directory(PyBytes_AsString(archive_bytes), self->zip_content);
        Py_DECREF(archive_bytes);

        if (files == NULL)
            return -1;
        if (PyDict_SetItem(zip_directory_cache, self->archive, files) != 0)
            return -1;
    }
    else
        Py_INCREF(files);
    self->files = files;

    Py_XDECREF(path_obj); // Not really used anymore
    return 0;
}

/* GC support. */
static int
zipimporter_traverse(PyObject *obj, visitproc visit, void *arg)
{
    ZipImporter *self = (ZipImporter *)obj;
    Py_VISIT(self->files);
    Py_VISIT(self->archive);
    Py_VISIT(self->prefix);
    Py_VISIT(self->zip_content);
    return 0;
}

static void
zipimporter_dealloc(ZipImporter *self)
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->archive);
    Py_XDECREF(self->prefix);
    Py_XDECREF(self->files);
    Py_XDECREF(self->zip_content);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
zipimporter_repr(ZipImporter *self)
{
    /* Use PyUnicode_FromFormat */
    if (self->archive == NULL)
         return PyUnicode_FromString("<zipimporter object \"???\">");

    if (self->prefix != NULL && PyUnicode_GetLength(self->prefix) > 0) {
        return PyUnicode_FromFormat("<zipimporter object \"%.300U%c%.150U\">",
                                    self->archive, SEP, self->prefix);
    } else {
        return PyUnicode_FromFormat("<zipimporter object \"%.300U\">",
                                    self->archive);
    }
}

/* return fullname.split(".")[-1] */
static const char *
get_subname(const char *fullname)
{
    const char *subname = strrchr(fullname, '.');
    if (subname == NULL)
        subname = fullname;
    else
        subname++;
    return subname;
}

/* Given a (sub)modulename, write the potential file path in the
   archive (without extension) to the path buffer. Return the
   length of the resulting string. */
static int
make_filename(const char *prefix, const char *name, char *path)
{
    size_t len;
    char *p;

    len = strlen(prefix);

    /* self.prefix + name [+ SEP + "__init__"] + ".py[co]" */
    if (len + strlen(name) + 13 >= MAXPATHLEN) {
        PyErr_SetString(ZipImportError, "path too long");
        return -1;
    }

    strcpy(path, prefix);
    strcpy(path + len, name);
    for (p = path + len; *p; p++) {
        if (*p == '.')
            *p = SEP;
    }
    len += strlen(name);
    return (int)len;
}

enum zi_module_info {
    MI_ERROR,
    MI_NOT_FOUND,
    MI_MODULE,
    MI_PACKAGE
};

/* Return some information about a module. */
static enum zi_module_info
get_module_info(ZipImporter *self, const char *fullname)
{
    const char *subname;
    char path[MAXPATHLEN + 1];
    int len;
    struct st_zip_searchorder *zso;

    /* We need prefix as C string (UTF-8) */
    PyObject *prefix_bytes = PyUnicode_AsUTF8String(self->prefix);
    if (!prefix_bytes) return MI_ERROR;
    const char *prefix_str = PyBytes_AsString(prefix_bytes);

    subname = get_subname(fullname);

    len = make_filename(prefix_str, subname, path);
    Py_DECREF(prefix_bytes);

    if (len < 0)
        return MI_ERROR;

    for (zso = zip_searchorder; *zso->suffix; zso++) {
        strcpy(path + len, zso->suffix);

        PyObject *path_obj = PyUnicode_FromString(path);
        if (!path_obj) return MI_ERROR;

        int exists = (PyDict_GetItem(self->files, path_obj) != NULL);
        Py_DECREF(path_obj);

        if (exists) {
            if (zso->type & IS_PACKAGE)
                return MI_PACKAGE;
            else
                return MI_MODULE;
        }
    }
    return MI_NOT_FOUND;
}

/* Check whether we can satisfy the import of the module named by
   'fullname'. Return self if we can, None if we can't. */
static PyObject *
zipimporter_find_module(PyObject *obj, PyObject *args)
{
    ZipImporter *self = (ZipImporter *)obj;
    PyObject *path = NULL;
    char *fullname;
    enum zi_module_info mi;

    if (!PyArg_ParseTuple(args, "s|O:zipimporter.find_module",
                          &fullname, &path))
        return NULL;

    mi = get_module_info(self, fullname);
    if (mi == MI_ERROR)
        return NULL;
    if (mi == MI_NOT_FOUND) {
        Py_INCREF(Py_None);
        return Py_None;
    }
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *
zipimporter_find_spec(PyObject *obj, PyObject *args, PyObject *kwds)
{
    ZipImporter *self = (ZipImporter *)obj;
    char *fullname;
    PyObject *target = NULL;
    enum zi_module_info mi;
    PyObject *spec = NULL;

    static PyObject *importlib_util = NULL;
    static PyObject *spec_from_loader = NULL;

    static char *kwlist[] = {"fullname", "target", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|O:zipimporter.find_spec", kwlist,
                          &fullname, &target))
        return NULL;

    mi = get_module_info(self, fullname);
    if (mi == MI_ERROR)
        return NULL;
    if (mi == MI_NOT_FOUND) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    if (spec_from_loader == NULL) {
        importlib_util = PyImport_ImportModule("importlib.util");
        if (importlib_util == NULL) return NULL;

        spec_from_loader = PyObject_GetAttrString(importlib_util, "spec_from_loader");
        Py_DECREF(importlib_util);
        if (spec_from_loader == NULL) return NULL;
    }

    spec = PyObject_CallFunction(spec_from_loader, "sO", fullname, self);

    return spec;
}

/* Load and return the module named by 'fullname'. */
static PyObject *
zipimporter_load_module(PyObject *obj, PyObject *args)
{
    ZipImporter *self = (ZipImporter *)obj;
    PyObject *code, *mod, *dict;
    char *fullname;
    PyObject *modpath_obj = NULL;
    int ispackage;

    if (!PyArg_ParseTuple(args, "s:zipimporter.load_module",
                          &fullname))
        return NULL;

    code = get_module_code(self, fullname, &ispackage, &modpath_obj);
    if (code == NULL)
        return NULL;

    mod = PyImport_AddModule(fullname);
    if (mod == NULL) {
        Py_DECREF(code);
        Py_XDECREF(modpath_obj);
        return NULL;
    }
    dict = PyModule_GetDict(mod);

    /* mod.__loader__ = self */
    if (PyDict_SetItemString(dict, "__loader__", (PyObject *)self) != 0)
        goto error;

    /* mod.__file__ = modpath */
    if (PyDict_SetItemString(dict, "__file__", modpath_obj) != 0)
        goto error;

    if (ispackage) {
        /* add __path__ to the module *before* the code gets
           executed */
        PyObject *pkgpath, *fullpath;
        const char *subname = get_subname(fullname);
        int err;

        fullpath = PyUnicode_FromFormat("%U%c%U%s",
                                self->archive,
                                SEP,
                                self->prefix,
                                subname);
        if (fullpath == NULL)
            goto error;

        pkgpath = Py_BuildValue("[O]", fullpath);
        Py_DECREF(fullpath);
        if (pkgpath == NULL)
            goto error;
        err = PyDict_SetItemString(dict, "__path__", pkgpath);
        Py_DECREF(pkgpath);
        if (err != 0)
            goto error;
    }

    PyObject *fullname_obj = PyUnicode_FromString(fullname);
    if (fullname_obj == NULL) {
        Py_DECREF(code);
        Py_XDECREF(modpath_obj);
        return NULL;
    }

    mod = PyImport_ExecCodeModuleObject(fullname_obj, code, modpath_obj, NULL);
    Py_DECREF(fullname_obj);

    Py_DECREF(code);
    Py_XDECREF(modpath_obj);

    if (mod) {
        /* Py_VerboseFlag is deprecated in 3.12, but we can't easily check verbose level
           without accessing internal state or using sys.flags which is Python object.
           We'll just omit the check or assume if people define specific macro.
           Actually PyConfig is the way now.
           Let's just use PySys_Audit if we wanted to be proper, or just remove verbose check
           or use the deprecated flag with warning suppression if we really want it.
           For now, let's remove the verbose output to be clean, or use sys.flags.verbose check.
        */
         /* Simplified: removed verbose check/output to avoid complications and warnings */
    }
    return mod;
error:
    Py_DECREF(code);
    Py_XDECREF(modpath_obj);
    Py_XDECREF(mod); /* PyImport_ExecCodeModuleObject returns new ref, but we failed before */
    return NULL;
}

/* Return a string matching __file__ for the named module */
static PyObject *
zipimporter_get_filename(PyObject *obj, PyObject *args)
{
    ZipImporter *self = (ZipImporter *)obj;
    PyObject *code;
    char *fullname;
    PyObject *modpath_obj = NULL;
    int ispackage;

    if (!PyArg_ParseTuple(args, "s:zipimporter.get_filename",
                         &fullname))
    return NULL;

    code = get_module_code(self, fullname, &ispackage, &modpath_obj);
    if (code == NULL)
        return NULL;
    Py_DECREF(code); /* Only need the path info */

    return modpath_obj;
}

/* Return a bool signifying whether the module is a package or not. */
static PyObject *
zipimporter_is_package(PyObject *obj, PyObject *args)
{
    ZipImporter *self = (ZipImporter *)obj;
    char *fullname;
    enum zi_module_info mi;

    if (!PyArg_ParseTuple(args, "s:zipimporter.is_package",
                          &fullname))
        return NULL;

    mi = get_module_info(self, fullname);
    if (mi == MI_ERROR)
        return NULL;
    if (mi == MI_NOT_FOUND) {
        PyErr_Format(ZipImportError, "can't find module '%.200s'",
                     fullname);
        return NULL;
    }
    return PyBool_FromLong(mi == MI_PACKAGE);
}

static PyObject *
zipimporter_get_data(PyObject *obj, PyObject *args)
{
    ZipImporter *self = (ZipImporter *)obj;
    char *path;
#ifdef ALTSEP
    char *p, buf[MAXPATHLEN + 1];
#endif
    PyObject *toc_entry;
    Py_ssize_t len;
    PyObject *path_obj;

    if (!PyArg_ParseTuple(args, "s:zipimporter.get_data", &path))
        return NULL;

#ifdef ALTSEP
    if (strlen(path) >= MAXPATHLEN) {
        PyErr_SetString(ZipImportError, "path too long");
        return NULL;
    }
    strcpy(buf, path);
    for (p = buf; *p; p++) {
        if (*p == ALTSEP)
            *p = SEP;
    }
    path = buf;
#endif
    /* self->archive is Unicode. We need to check if path starts with archive */
    /* Convert archive to UTF8 char* for comparison? */
    /* Or convert path to Unicode? */

    path_obj = PyUnicode_FromString(path);
    if (!path_obj) return NULL;

    /* Check if path starts with archive */
    /* Note: path might be relative inside archive, or full path */

    if (PyUnicode_CompareWithASCIIString(path_obj, "") == 0) {
        /* Empty path? */
    }

    /* Simplified logic: users often pass full path. */
    /* if path.startswith(self.archive + SEP) */

    /* Optimization: Just try to look it up first? */
    /* But we need to support stripping the archive prefix if present. */

    /* Let's convert archive to string for strncmp logic as before, for simplicity */
    PyObject *archive_bytes = PyUnicode_AsUTF8String(self->archive);
    const char *archive_str = PyBytes_AsString(archive_bytes);

    len = strlen(archive_str);
    if ((size_t)len < strlen(path) &&
        strncmp(path, archive_str, len) == 0 &&
        path[len] == SEP) {
        path = path + len + 1;
    }
    Py_DECREF(archive_bytes);

    PyObject *key = PyUnicode_FromString(path);
    if (!key) { Py_DECREF(path_obj); return NULL; }

    toc_entry = PyDict_GetItem(self->files, key);
    Py_DECREF(key);

    if (toc_entry == NULL) {
        PyErr_SetFromErrnoWithFilename(PyExc_IOError, path);
        Py_DECREF(path_obj);
        return NULL;
    }
    Py_DECREF(path_obj);

    /* get_data expects archive path as char* for error msg */
    return get_data("???", self->zip_content, toc_entry);
}

static PyObject *
zipimporter_get_code(PyObject *obj, PyObject *args)
{
    ZipImporter *self = (ZipImporter *)obj;
    char *fullname;

    if (!PyArg_ParseTuple(args, "s:zipimporter.get_code", &fullname))
        return NULL;

    return get_module_code(self, fullname, NULL, NULL);
}

static PyObject *
zipimporter_get_source(PyObject *obj, PyObject *args)
{
    ZipImporter *self = (ZipImporter *)obj;
    PyObject *toc_entry;
    char *fullname;
    const char *subname;
    char path[MAXPATHLEN+1];
    int len;
    enum zi_module_info mi;

    if (!PyArg_ParseTuple(args, "s:zipimporter.get_source", &fullname))
        return NULL;

    mi = get_module_info(self, fullname);
    if (mi == MI_ERROR)
        return NULL;
    if (mi == MI_NOT_FOUND) {
        PyErr_Format(ZipImportError, "can't find module '%.200s'",
                     fullname);
        return NULL;
    }
    subname = get_subname(fullname);

    PyObject *prefix_bytes = PyUnicode_AsUTF8String(self->prefix);
    const char *prefix_str = PyBytes_AsString(prefix_bytes);

    len = make_filename(prefix_str, subname, path);
    Py_DECREF(prefix_bytes);

    if (len < 0)
        return NULL;

    if (mi == MI_PACKAGE) {
        path[len] = SEP;
        strcpy(path + len + 1, "__init__.py");
    }
    else
        strcpy(path + len, ".py");

    PyObject *key = PyUnicode_FromString(path);
    toc_entry = PyDict_GetItem(self->files, key);
    Py_DECREF(key);

    if (toc_entry != NULL)
        return get_data("???", self->zip_content, toc_entry);

    /* we have the module, but no source */
    Py_INCREF(Py_None);
    return Py_None;
}

static PyMethodDef zipimporter_methods[] = {
    {"find_module", zipimporter_find_module, METH_VARARGS, NULL},
    {"find_spec", (PyCFunction)zipimporter_find_spec, METH_VARARGS | METH_KEYWORDS, NULL},
    {"load_module", zipimporter_load_module, METH_VARARGS, NULL},
    {"get_data", zipimporter_get_data, METH_VARARGS, NULL},
    {"get_code", zipimporter_get_code, METH_VARARGS, NULL},
    {"get_source", zipimporter_get_source, METH_VARARGS, NULL},
    {"get_filename", zipimporter_get_filename, METH_VARARGS, NULL},
    {"is_package", zipimporter_is_package, METH_VARARGS, NULL},
    {NULL,              NULL}   /* sentinel */
};

static PyMemberDef zipimporter_members[] = {
    {"archive",  T_OBJECT, offsetof(ZipImporter, archive),  READONLY},
    {"prefix",   T_OBJECT, offsetof(ZipImporter, prefix),   READONLY},
    {"_files",   T_OBJECT, offsetof(ZipImporter, files),    READONLY},
    {NULL}
};

static PyTypeObject ZipImporter_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "memzipimport.zipimporter",
    sizeof(ZipImporter),
    0,                                          /* tp_itemsize */
    (destructor)zipimporter_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    (reprfunc)zipimporter_repr,                 /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_HAVE_GC,                     /* tp_flags */
    NULL,                            /* tp_doc */
    zipimporter_traverse,                       /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    zipimporter_methods,                        /* tp_methods */
    zipimporter_members,                        /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    (initproc)zipimporter_init,                 /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    PyType_GenericNew,                          /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
};


/* implementation */

/* Given a buffer, return the long that is represented by the first
   4 bytes, encoded as little endian. This partially reimplements
   marshal.c:r_long() */
static long
get_long(unsigned char *buf) {
    long x;
    x =  buf[0];
    x |= (long)buf[1] <<  8;
    x |= (long)buf[2] << 16;
    x |= (long)buf[3] << 24;
#if SIZEOF_LONG > 4
    /* Sign extension for 64-bit machines */
    x |= -(x & 0x80000000L);
#endif
    return x;
}

static short
get_short(unsigned char *buf) {
    short x;
    x =  buf[0];
    x |= (long)buf[1] <<  8;
    return x;
}

/*
   read_directory(archive, zip_content) -> files dict (new reference)
*/
static PyObject *
read_directory(const char *archive, PyObject *zip_content)
{
    PyObject *files = NULL;
    long compress, crc, data_size, file_size, file_offset, date, time;
    long header_offset, name_size, header_size, header_position;
    long i, l, count;
    char path[MAXPATHLEN + 5];
    char name[MAXPATHLEN + 5];
    char *p;
    unsigned char *content, *endof_central_dir, *header, *file;
    long arc_offset; /* offset from beginning of file to start of zip-archive */
    Py_ssize_t content_size;

    char *c_content;

    if (PyByteArray_Check(zip_content)) {
        c_content = PyByteArray_AsString(zip_content);
        content_size = PyByteArray_Size(zip_content);
    } else if (PyBytes_Check(zip_content)) {
        c_content = PyBytes_AsString(zip_content);
        content_size = PyBytes_Size(zip_content);
    } else {
         PyErr_SetString(PyExc_TypeError, "zip_content must be bytes or bytearray");
         return NULL;
    }

    content = (unsigned char *)c_content;

    if (content_size < 22) {
        PyErr_Format(ZipImportError, "can't read Zip file: "
                     "'%.200s'", archive);
        return NULL;
    }

    endof_central_dir = content + content_size - 22;
    header_position = content_size - 22;

    if (get_long((unsigned char *)endof_central_dir) != 0x06054B50) {
        /* Bad: End of Central Dir signature */
        PyErr_Format(ZipImportError, "not a Zip file: "
                     "'%.200s'", archive);
        return NULL;
    }

    header_size = get_long(endof_central_dir + 12);
    header_offset = get_long(endof_central_dir + 16);
    arc_offset = header_position - header_offset - header_size;
    header_offset += arc_offset;

    files = PyDict_New();
    if (files == NULL)
        goto error;

    /* OPTIMIZATION: path prefix reuse */
    /* archive is typically just the zip filename here if we are careful,
       but read_directory is called with 'buf' which is full path. */
    if (strlen(archive) > MAXPATHLEN) {
        PyErr_SetString(PyExc_OverflowError, "Zip path name is too long");
        goto error;
    }
    strcpy(path, archive);
    size_t length = strlen(path);
    path[length] = SEP;
    /* path now: "/path/to/archive.zip/" */

    /* Start of Central Directory */
    count = 0;
    for (;;) {
        PyObject *t;
        int err;

        if (header_offset > content_size - 4) break; /* safety */

        l = get_long(content + header_offset);
        if (l != 0x02014B50)
            break;              /* Bad: Central Dir File Header */

        header = content + header_offset + 10;
        compress = get_short(header); header += 2;
        time = get_short(header); header += 2;
        date = get_short(header); header += 2;
        crc = get_long(header); header += 4;
        data_size = get_long(header); header += 4;
        file_size = get_long(header); header += 4;
        name_size = get_short(header); header += 2;
        header_size = 46 + name_size +
           get_short(header) +
           get_short(header +2);

        file_offset = get_long(content + header_offset + 42) + arc_offset;

        if (name_size > MAXPATHLEN) {
             /* SAFETY: Don't truncate, just skip or error. Skip is better? */
             /* Or error. Standard zipimport doesn't seem to error, just truncate? */
             /* We will skip to avoid memory corruption or weirdness. */
             header_offset += header_size;
             continue;
        }

        file = content + header_offset + 42 + 4;
        p = name;
        for (i = 0; i < name_size; i++) {
            *p = *file;
            if (*p == '/')
                *p = SEP;
            p++;
            file++;
        }
        *p = 0;         /* Add terminating null byte */
        header_offset += header_size;

        /* OPTIMIZATION: Avoid Py_BuildValue in loop */
        /* Tuple: (path, compress, data_size, file_size, file_offset, time, date, crc) */

        t = PyTuple_New(8);
        if (t == NULL) goto error;

        /* Construct full path: archive + SEP + name */
        /* Optimization: Create PyUnicode from 'path' (prefix) and 'name' efficiently? */
        /* For now, just construct the C string and convert. */
        /* length is len(archive) + 1 (SEP) */
        if (length + name_size >= MAXPATHLEN) {
             Py_DECREF(t);
             continue; // Skip too long paths
        }
        strcpy(path + length + 1, name);

        PyObject *u_path = PyUnicode_FromString(path);
        if (u_path == NULL) { Py_DECREF(t); goto error; }

        PyTuple_SET_ITEM(t, 0, u_path);
        PyTuple_SET_ITEM(t, 1, PyLong_FromLong(compress));
        PyTuple_SET_ITEM(t, 2, PyLong_FromLong(data_size));
        PyTuple_SET_ITEM(t, 3, PyLong_FromLong(file_size));
        PyTuple_SET_ITEM(t, 4, PyLong_FromLong(file_offset));
        PyTuple_SET_ITEM(t, 5, PyLong_FromLong(time));
        PyTuple_SET_ITEM(t, 6, PyLong_FromLong(date));
        PyTuple_SET_ITEM(t, 7, PyLong_FromLong(crc));

        PyObject *u_name = PyUnicode_FromString(name);
        if (u_name == NULL) { Py_DECREF(t); goto error; }

        err = PyDict_SetItem(files, u_name, t);
        Py_DECREF(u_name);
        Py_DECREF(t);
        if (err != 0)
            goto error;
        count++;
    }
    return files;
error:
    Py_XDECREF(files);
    return NULL;
}

static PyObject *
get_decompress_func(void)
{
    static PyObject *decompress = NULL;

    if (decompress == NULL) {
        PyObject *zlib;
        static int importing_zlib = 0;

        if (importing_zlib != 0)
            return NULL;
        importing_zlib = 1;
        zlib = PyImport_ImportModule("zlib");
        importing_zlib = 0;
        if (zlib != NULL) {
            decompress = PyObject_GetAttrString(zlib,
                                                "decompress");
            Py_DECREF(zlib);
        }
        else
            PyErr_Clear();
    }
    return decompress;
}

static PyObject *
get_data(const char *archive, PyObject *zip_content, PyObject *toc_entry)
{
    PyObject *raw_data, *data = NULL, *decompress;
    char *buf;
    long l;
    PyObject *datapath_obj;
    long compress, data_size, file_size, file_offset;
    long time, date, crc;
    char *content, *file;
    Py_ssize_t content_size;

    if (!PyArg_ParseTuple(toc_entry, "Olllllll", &datapath_obj, &compress,
                          &data_size, &file_size, &file_offset, &time,
                          &date, &crc)) {
        return NULL;
    }

    if (PyByteArray_Check(zip_content)) {
        content = PyByteArray_AsString(zip_content);
        content_size = PyByteArray_Size(zip_content);
    } else if (PyBytes_Check(zip_content)) {
        content = PyBytes_AsString(zip_content);
        content_size = PyBytes_Size(zip_content);
    } else {
        return NULL;
    }

    l = get_long((unsigned char *)content);
    if (l != 0x04034B50) {
        /* Bad: Local File Header */
        PyErr_Format(ZipImportError,
                     "bad local file header in %s",
                     archive);
        return NULL;
    }
    file = content + file_offset + 26;
    l = 30 + get_short((unsigned char*)file) +
        get_short((unsigned char*)file + 2);        /* local header size */
    file_offset += l;           /* Start of file data */

    if ((file_offset + data_size) > content_size) {
        PyErr_SetString(PyExc_IOError,
                        "zipimport: can't read data");
        return NULL;
    }

    /* Allocate bytes object */
    raw_data = PyBytes_FromStringAndSize((char *)NULL, compress == 0 ?
                                          data_size : data_size + 1);
    if (raw_data == NULL) {
        return NULL;
    }
    buf = PyBytes_AsString(raw_data);

    memcpy(buf, content + file_offset, data_size);

    if (compress != 0) {
        buf[data_size] = 'Z';  /* saw this in zipfile.py */
        data_size++;
    }
    // buf[data_size] = '\0'; // Not needed for Bytes object? PyBytes_FromStringAndSize adds null? Yes.

    if (compress == 0)  /* data is not compressed */
        return raw_data;

    /* Decompress with zlib */
    decompress = get_decompress_func();
    if (decompress == NULL) {
        PyErr_SetString(ZipImportError,
                        "can't decompress data; "
                        "zlib not available");
        goto error;
    }
    data = PyObject_CallFunction(decompress, "Oi", raw_data, -15);
error:
    Py_DECREF(raw_data);
    return data;
}

static int
eq_mtime(time_t t1, time_t t2)
{
    time_t d = t1 - t2;
    if (d < 0)
        d = -d;
    /* dostime only stores even seconds, so be lenient */
    return d <= 1;
}

static PyObject *
unmarshal_code(const char *pathname, PyObject *data, time_t mtime)
{
    PyObject *code;
    char *buf = PyBytes_AsString(data);
    Py_ssize_t size = PyBytes_Size(data);

    if (size <= 16) { /* 16 bytes for header in Py3.7+? Check magic */
        PyErr_SetString(ZipImportError,
                        "bad pyc data");
        return NULL;
    }

    /* Python 3 .pyc header changes over versions.
       Usually: 4 bytes magic, 4 bytes flags/bitfield, 4 bytes timestamp, 4 bytes size.
       Total 16 bytes.
    */

    if (get_long((unsigned char *)buf) != PyImport_GetMagicNumber()) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    if (mtime != 0 && !eq_mtime(get_long((unsigned char *)buf + 8),
                                mtime)) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    code = PyMarshal_ReadObjectFromString(buf + 16, size - 16);
    if (code == NULL)
        return NULL;
    if (!PyCode_Check(code)) {
        Py_DECREF(code);
        PyErr_Format(PyExc_TypeError,
             "compiled module %.200s is not a code object",
             pathname);
        return NULL;
    }
    return code;
}

static PyObject *
normalize_line_endings(PyObject *source)
{
    char *buf, *q, *p = PyBytes_AsString(source);
    PyObject *fixed_source;

    if (!p)
        return NULL;

    /* one char extra for trailing \n and one for terminating \0 */
    buf = (char *)PyMem_Malloc(PyBytes_Size(source) + 2);
    if (buf == NULL) {
        PyErr_SetString(PyExc_MemoryError,
                        "zipimport: no memory to allocate "
                        "source buffer");
        return NULL;
    }
    /* replace "\r\n?" by "\n" */
    for (q = buf; *p != '\0'; p++) {
        if (*p == '\r') {
            *q++ = '\n';
            if (*(p + 1) == '\n')
                p++;
        }
        else
            *q++ = *p;
    }
    *q++ = '\n';  /* add trailing \n */
    *q = '\0';
    fixed_source = PyBytes_FromString(buf);
    PyMem_Free(buf);
    return fixed_source;
}

static PyObject *
compile_source(const char *pathname, PyObject *source)
{
    PyObject *code, *fixed_source;

    fixed_source = normalize_line_endings(source);
    if (fixed_source == NULL)
        return NULL;

    code = Py_CompileString(PyBytes_AsString(fixed_source), pathname,
                            Py_file_input);
    Py_DECREF(fixed_source);
    return code;
}

static time_t
parse_dostime(int dostime, int dosdate)
{
    struct tm stm;

    memset((void *) &stm, '\0', sizeof(stm));

    stm.tm_sec   =  (dostime        & 0x1f) * 2;
    stm.tm_min   =  (dostime >> 5)  & 0x3f;
    stm.tm_hour  =  (dostime >> 11) & 0x1f;
    stm.tm_mday  =   dosdate        & 0x1f;
    stm.tm_mon   = ((dosdate >> 5)  & 0x0f) - 1;
    stm.tm_year  = ((dosdate >> 9)  & 0x7f) + 80;
    stm.tm_isdst =   -1; /* wday/yday is ignored */

    return mktime(&stm);
}

static time_t
get_mtime_of_source(ZipImporter *self, char *path)
{
    PyObject *toc_entry;
    time_t mtime = 0;
    Py_ssize_t lastchar = strlen(path) - 1;
    char savechar = path[lastchar];
    path[lastchar] = '\0';  /* strip 'c' or 'o' from *.py[co] */

    PyObject *key = PyUnicode_FromString(path);
    toc_entry = PyDict_GetItem(self->files, key);
    Py_DECREF(key);

    if (toc_entry != NULL && PyTuple_Check(toc_entry) &&
        PyTuple_Size(toc_entry) == 8) {
        int time, date;
        time = PyLong_AsLong(PyTuple_GetItem(toc_entry, 5));
        date = PyLong_AsLong(PyTuple_GetItem(toc_entry, 6));
        mtime = parse_dostime(time, date);
    }
    path[lastchar] = savechar;
    return mtime;
}

static PyObject *
get_code_from_data(ZipImporter *self, int ispackage, int isbytecode,
                   time_t mtime, PyObject *toc_entry)
{
    PyObject *data, *code;
    PyObject *modpath_obj;
    const char *modpath;

    /* Archive name not strictly needed here for get_data call */

    data = get_data("???", self->zip_content, toc_entry);
    if (data == NULL)
        return NULL;

    modpath_obj = PyTuple_GetItem(toc_entry, 0);
    PyObject *modpath_bytes = PyUnicode_AsUTF8String(modpath_obj);
    if (!modpath_bytes) { Py_DECREF(data); return NULL; }

    modpath = PyBytes_AsString(modpath_bytes);

    if (isbytecode) {
        code = unmarshal_code(modpath, data, mtime);
    }
    else {
        code = compile_source(modpath, data);
    }
    Py_DECREF(data);
    Py_DECREF(modpath_bytes);
    return code;
}

static PyObject *
get_module_code(ZipImporter *self, const char *fullname,
                int *p_ispackage, PyObject **p_modpath)
{
    PyObject *toc_entry;
    char path[MAXPATHLEN + 1];
    int len;
    struct st_zip_searchorder *zso;
    const char *subname = get_subname(fullname);

    PyObject *prefix_bytes = PyUnicode_AsUTF8String(self->prefix);
    const char *prefix_str = PyBytes_AsString(prefix_bytes);

    len = make_filename(prefix_str, subname, path);
    Py_DECREF(prefix_bytes);

    if (len < 0)
        return NULL;

    for (zso = zip_searchorder; *zso->suffix; zso++) {
        PyObject *code = NULL;

        strcpy(path + len, zso->suffix);

        PyObject *key = PyUnicode_FromString(path);
        toc_entry = PyDict_GetItem(self->files, key);
        Py_DECREF(key);

        if (toc_entry != NULL) {
            time_t mtime = 0;
            int ispackage = zso->type & IS_PACKAGE;
            int isbytecode = zso->type & IS_BYTECODE;

            if (isbytecode)
                mtime = get_mtime_of_source(self, path);
            if (p_ispackage != NULL)
                *p_ispackage = ispackage;
            code = get_code_from_data(self, ispackage,
                                      isbytecode, mtime,
                                      toc_entry);
            if (code == Py_None) {
                Py_DECREF(code);
                continue;
            }
            if (code != NULL && p_modpath != NULL) {
                *p_modpath = PyTuple_GetItem(toc_entry, 0);
                Py_INCREF(*p_modpath);
            }
            return code;
        }
    }
    PyErr_Format(ZipImportError, "can't find module '%.200s'", fullname);
    return NULL;
}


/* Module init */

static PyModuleDef memzipimportmodule = {
    PyModuleDef_HEAD_INIT,
    "memzipimport",
    NULL,
    -1,
    NULL, NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC
PyInit_memzipimport(void)
{
    PyObject *mod;

    if (PyType_Ready(&ZipImporter_Type) < 0)
        return NULL;

    /* Correct directory separator */
    zip_searchorder[0].suffix[0] = SEP;
    zip_searchorder[1].suffix[0] = SEP;
    zip_searchorder[2].suffix[0] = SEP;

    mod = PyModule_Create(&memzipimportmodule);
    if (mod == NULL)
        return NULL;

    ZipImportError = PyErr_NewException("memzipimport.ZipImportError",
                                        PyExc_ImportError, NULL);
    if (ZipImportError == NULL)
        return NULL;

    Py_INCREF(ZipImportError);
    if (PyModule_AddObject(mod, "ZipImportError",
                           ZipImportError) < 0)
        return NULL;

    Py_INCREF(&ZipImporter_Type);
    if (PyModule_AddObject(mod, "zipimporter",
                           (PyObject *)&ZipImporter_Type) < 0)
        return NULL;

    zip_directory_cache = PyDict_New();
    if (zip_directory_cache == NULL)
        return NULL;
    Py_INCREF(zip_directory_cache);
    if (PyModule_AddObject(mod, "_zip_directory_cache",
                           zip_directory_cache) < 0)
        return NULL;

    return mod;
}
