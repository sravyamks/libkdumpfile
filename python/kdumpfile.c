#include <Python.h>
#include <kdumpfile.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
	PyObject_HEAD
	kdump_ctx *ctx;
	PyObject *file;
	PyObject *cb_get_symbol;
} kdumpfile_object;

static PyObject *SysErrException;
static PyObject *UnsupportedException;
static PyObject *NoDataException;
static PyObject *DataErrException;
static PyObject *InvalidException;
static PyObject *NoKeyException;
static PyObject *EOFException;

static PyObject *
exception_map(kdump_status status)
{
	switch (status) {
	case kdump_syserr:      return SysErrException;
	case kdump_unsupported: return UnsupportedException;
	case kdump_nodata:      return NoDataException;
	case kdump_dataerr:     return DataErrException;
	case kdump_invalid:     return InvalidException;
	case kdump_nokey:       return NoKeyException;
	case kdump_eof:         return EOFException;
	/* If we raise an exception with status == kdump_ok, it's a bug. */
	case kdump_ok:
	default:                return PyExc_RuntimeError;
	};
}

static kdump_status cb_get_symbol(kdump_ctx *ctx, const char *name, kdump_addr_t *addr)
{
	kdumpfile_object *self;
	PyObject *ret;

	self = (kdumpfile_object*)kdump_get_priv(ctx);

	if (! self->cb_get_symbol) {
		PyErr_SetString(PyExc_RuntimeError, "Callback symbol-resolving function not set");
		printf ("aa\n");
		return kdump_nodata;
	}

	ret = PyObject_CallFunction(self->cb_get_symbol, "s", name);

	if (! PyLong_Check(ret)) {
		PyErr_SetString(PyExc_RuntimeError, "Callback of symbol-resolving function returned no long");
		printf ("bb\n");
		return kdump_nodata;
	}

	*addr = PyLong_AsUnsignedLongLong(ret);;

	Py_XDECREF(ret);

	return kdump_ok;
}

static PyObject *
kdumpfile_new (PyTypeObject *type, PyObject *args, PyObject *kw)
{
	kdumpfile_object *self = NULL;
	static char *keywords[] = {"file", NULL};
	kdump_status status;
	PyObject *fo = NULL;
	int fd;

	if (!PyArg_ParseTupleAndKeywords (args, kw, "O!", keywords,
				    &PyFile_Type, &fo))
		    return NULL;

	self = (kdumpfile_object*) type->tp_alloc (type, 0);
	if (!self)
		return NULL;

	self->ctx = kdump_alloc_ctx();
	if (!self->ctx) {
		PyErr_SetString(PyExc_MemoryError,
				"Couldn't allocate kdump context");
		goto fail;
	}

	status = kdump_init_ctx(self->ctx);
	if (status != kdump_ok) {
		PyErr_Format(SysErrException,
			     "Couldn't initialize kdump context: %s",
			     kdump_err_str(self->ctx));
		goto fail;
	}

	fd = fileno(PyFile_AsFile(fo));
	status = kdump_set_fd(self->ctx, fd);
	if (status != kdump_ok) {
		PyErr_Format(exception_map(status),
			     "Cannot open dump: %s", kdump_err_str(self->ctx));
		goto fail;
	}

	self->file = fo;
	Py_INCREF(fo);

	self->cb_get_symbol = NULL;
	kdump_cb_get_symbol_val(self->ctx, cb_get_symbol);
	kdump_set_priv(self->ctx, self);

	return (PyObject*)self;

fail:
	Py_XDECREF(self);
	return NULL;
}

static void
kdumpfile_dealloc(PyObject *_self)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	if (self->ctx) {
		kdump_free(self->ctx);
		self->ctx = NULL;
	}

	if (self->file) Py_XDECREF(self->file);
	self->ob_type->tp_free((PyObject*)self);
}

static PyObject *kdumpfile_read (PyObject *_self, PyObject *args, PyObject *kw)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;
	PyObject *obj;
	kdump_paddr_t addr;
	kdump_status status;
	int addrspace;
	unsigned long size;
	static char *keywords[] = {"addrspace", "address", "size", NULL};
	size_t r;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "ikk:",
					 keywords, &addrspace, &addr, &size))
		return NULL;

	if (!size) {
		PyErr_SetString(PyExc_ValueError, "Zero size buffer");
		return NULL;
	}

	obj = PyByteArray_FromStringAndSize(0, size);
	if (!obj)
		return NULL;

	r = size;
	status = kdump_readp(self->ctx, addrspace, addr,
			     PyByteArray_AS_STRING(obj), &r);
	if (status != kdump_ok) {
		Py_XDECREF(obj);
		PyErr_Format(exception_map(status), kdump_err_str(self->ctx));
		return NULL;
	}

	if (r != size) {
		Py_XDECREF(obj);
		PyErr_Format(PyExc_IOError,
			     "Got %d bytes, expected %d bytes: %s",
			     r, size, kdump_err_str(self->ctx));
		return NULL;
	}
	return obj;
}

struct attr2obj_data {
	kdump_ctx *ctx;
	PyObject *dict;
};

static PyObject *kdumpfile_attr2obj(kdump_ctx *ctx, const struct kdump_attr *attr);

static int kdumpfile_dir2obj_it(void *data, const char *key, const struct kdump_attr *valp)
{
	struct attr2obj_data *cb_data = (struct attr2obj_data *)data;
	PyObject *v = kdumpfile_attr2obj(cb_data->ctx, valp);
	if (! v) return 1;
	PyDict_SetItem(cb_data->dict, PyString_FromString(key), v);
	return 0;
}

static PyObject *kdumpfile_dir2obj(kdump_ctx *ctx, const struct kdump_attr *attr)
{
	struct attr2obj_data cb_data;

	cb_data.ctx = ctx;
	cb_data.dict = PyDict_New();

	if (kdump_enum_attr_val(ctx, attr, kdumpfile_dir2obj_it, &cb_data)) {
		Py_XDECREF(cb_data.dict);
		return NULL;
	}

	return cb_data.dict;
}

static PyObject *kdumpfile_attr2obj(kdump_ctx *ctx, const struct kdump_attr *attr)
{
	switch (attr->type) {
		case kdump_number:
			return PyLong_FromUnsignedLong(attr->val.number);
		case kdump_address:
			return PyLong_FromUnsignedLong(attr->val.address);
		case kdump_string:
			return PyString_FromString(attr->val.string);
		case kdump_directory:
			return kdumpfile_dir2obj(ctx, attr);
		default:
			PyErr_SetString(PyExc_RuntimeError, "Unhandled attr type");
			return NULL;
	}
}

static PyObject *kdumpfile_getattr(PyObject *_self, PyObject *args, PyObject *kw)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;
	static char *keywords[] = {"name", NULL};
	struct kdump_attr attr;
	const char *name;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "s:", keywords, &name))
		return NULL;

	if (kdump_get_attr(self->ctx, name, &attr) != kdump_ok) {
		Py_RETURN_NONE;
	}

	return kdumpfile_attr2obj(self->ctx, &attr);
}


static PyObject *kdumpfile_vtop_init(PyObject *_self, PyObject *args)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	kdump_vtop_init(self->ctx);

	Py_RETURN_NONE;
}

static PyMethodDef kdumpfile_object_methods[] = {
	{"read",      (PyCFunction) kdumpfile_read, METH_VARARGS | METH_KEYWORDS,
		"read (addrtype, address) -> buffer.\n" },
	{"attr",      (PyCFunction) kdumpfile_getattr, METH_VARARGS | METH_KEYWORDS,
		"Get dump attribute: attr(name) -> value.\n"},
	{"vtop_init", (PyCFunction) kdumpfile_vtop_init, METH_NOARGS,
		"Initialize virtual memory mapping\n"},
	{NULL}  
};

static PyObject *kdumpfile_getconst (PyObject *_self, void *_value)
{
	return PyInt_FromLong((long)_value);
}

static PyObject *kdumpfile_get_symbol_func (PyObject *_self, void *_data)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	if (! self->cb_get_symbol)
		Py_RETURN_NONE;
	Py_INCREF(self->cb_get_symbol);

	return self->cb_get_symbol;
}

int kdumpfile_set_symbol_func(PyObject *_self, PyObject *_set, void *_data)
{
	kdumpfile_object *self = (kdumpfile_object*)_self;

	if (! PyCallable_Check(_set)) {
		PyErr_SetString(PyExc_RuntimeError, "Argument must be callable");
		return 1;
	}

	if (self->cb_get_symbol)
		Py_XDECREF(self->cb_get_symbol);
	self->cb_get_symbol = _set;
	Py_INCREF(self->cb_get_symbol);
	return 0;
}

static void
cleanup_exceptions(void)
{
	Py_XDECREF(SysErrException);
	Py_XDECREF(UnsupportedException);
	Py_XDECREF(NoDataException);
	Py_XDECREF(DataErrException);
	Py_XDECREF(InvalidException);
	Py_XDECREF(NoKeyException);
	Py_XDECREF(EOFException);
}

static int lookup_exceptions (void)
{
	PyObject *mod = PyImport_ImportModule("kdumpfile.exceptions");
	if (!mod)
		return -1;

#define lookup_exception(name)				\
do {							\
	name = PyObject_GetAttrString(mod, #name);	\
	if (!name)					\
		goto fail;				\
} while(0)

	lookup_exception(SysErrException);
	lookup_exception(UnsupportedException);
	lookup_exception(NoDataException);
	lookup_exception(DataErrException);
	lookup_exception(InvalidException);
	lookup_exception(EOFException);
#undef lookup_exception

	Py_XDECREF(mod);
	return 0;
fail:
	cleanup_exceptions();
	Py_XDECREF(mod);
	return -1;
}

static PyGetSetDef kdumpfile_object_getset[] = {
	{"KDUMP_KPHYSADDR", kdumpfile_getconst, NULL, 
		"KDUMP_KPHYSADDR - get by kernel physical address",
		(void*)KDUMP_KPHYSADDR},
	{"KDUMP_MACHPHYSADDR", kdumpfile_getconst, NULL, 
		"KDUMP_MACHPHYSADDR - get by machine physical address",
		(void*)KDUMP_MACHPHYSADDR},
	{"KDUMP_KVADDR", kdumpfile_getconst, NULL, 
		"KDUMP_KVADDR - get by kernel virtual address",
		(void*)KDUMP_KVADDR},
	{"KDUMP_XENVADDR", kdumpfile_getconst, NULL, 
		"KDUMP_XENKVADDR - get by xen virtual address",
		(void*)KDUMP_XENVADDR},
	{"symbol_func", kdumpfile_get_symbol_func, kdumpfile_set_symbol_func,
		"Callback function called by libkdumpfile for symbol resolving",
		NULL},
	{NULL}
};

static PyTypeObject kdumpfile_object_type = 
{
	PyObject_HEAD_INIT (0) 
	0,
	"_kdumpfile.kdumpfile",         /* tp_name*/ 
	sizeof (kdumpfile_object),      /* tp_basicsize*/ 
	0,                              /* tp_itemsize*/ 
	kdumpfile_dealloc,              /* tp_dealloc*/ 
	0,                              /* tp_print*/ 
	0,                              /* tp_getattr*/ 
	0,                              /* tp_setattr*/ 
	0,                              /* tp_compare*/ 
	0,                              /* tp_repr*/ 
	0,                              /* tp_as_number*/ 
	0,                              /* tp_as_sequence*/
	0,                              /* tp_as_mapping*/ 
	0,                              /* tp_hash */ 
	0,                              /* tp_call*/ 
	0,                              /* tp_str*/ 
	0,                              /* tp_getattro*/ 
	0,                              /* tp_setattro*/ 
	0,                              /* tp_as_buffer*/ 
	Py_TPFLAGS_DEFAULT,             /* tp_flags*/ 
	"kdumpfile",                    /* tp_doc */ 
	0,                              /* tp_traverse */ 
	0,                              /* tp_clear */ 
	0,                              /* tp_richcompare */ 
	0,                              /* tp_weaklistoffset */ 
	0,                              /* tp_iter */ 
	0,                              /* tp_iternext */ 
	kdumpfile_object_methods,       /* tp_methods */ 
	0,                              /* tp_members */ 
	kdumpfile_object_getset,        /* tp_getset */ 
	0,                              /* tp_base */
	0,                              /* tp_dict */
	0,                              /* tp_descr_get */
	0,                              /* tp_descr_set */
	0, 				  /* tp_dictoffset */
	0,                              /* tp_init */
	0,                              /* tp_alloc */
	kdumpfile_new,                  /* tp_new */
};

PyMODINIT_FUNC
init_kdumpfile (void)
{
	PyObject *mod;
	int ret;

	if (PyType_Ready(&kdumpfile_object_type) < 0) 
		return;

	ret = lookup_exceptions();
	if (ret)
		return;

	mod = Py_InitModule3("_kdumpfile", NULL,
			"kdumpfile - interface to libkdumpfile");
	if (!mod)
		goto fail;

	Py_INCREF((PyObject *)&kdumpfile_object_type);
	ret = PyModule_AddObject(mod, "kdumpfile",
				 (PyObject*)&kdumpfile_object_type);
	if (ret)
		goto fail;
	return;
fail:
	cleanup_exceptions();
	Py_XDECREF(mod);
}
