
#include <Python.h>
#include "replace.h"
#include "zfsacl.h"

#define Py_TPFLAGS_HAVE_ITER 0

typedef struct {
        PyObject_HEAD
	bool verbose;
	zfsacl_t theacl;
} py_acl;

typedef struct {
        PyObject_HEAD
	py_acl *parent_acl;
	int idx;
	uint initial_cnt;
	zfsacl_entry_t theace;
} py_acl_entry;

typedef struct {
	PyObject_HEAD
	py_acl *acl;
	int current_idx;
} py_acl_iterator;

static void set_exc_from_errno(const char *func)
{
	PyErr_Format(
		PyExc_RuntimeError,
		"%s failed: %s", func, strerror(errno)
	);
}

static PyObject *py_acl_iter_next(py_acl_iterator *self)
{
	PyObject *out = NULL;

	out = PyObject_CallMethod(
		(PyObject *)self->acl, "get_entry", "i", self->current_idx
	);
	if (out == NULL) {
		if (PyErr_Occurred() == NULL) {
			return NULL;
		}
		if (PyErr_ExceptionMatches(PyExc_IndexError)) {
			/* iteration done */
			PyErr_Clear();
			PyErr_SetNone(PyExc_StopIteration);
			return NULL;
		}
		/* Some other error occurred */
		return NULL;
	}

	self->current_idx++;
	return out;
}

static void py_acl_iter_dealloc(py_acl_iterator *self)
{
	Py_CLEAR(self->acl);
	PyObject_Del(self);
}

PyTypeObject PyACLIterator = {
	.tp_name = "ACL Iterator",
	.tp_basicsize = sizeof(py_acl_iterator),
	.tp_iternext = (iternextfunc)py_acl_iter_next,
	.tp_dealloc = (destructor)py_acl_iter_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_iter = PyObject_SelfIter,
};

static PyObject *aclflag_to_pylist(zfsacl_aclflags_t flags)
{
	int i, err;
	PyObject *out = NULL;

	out = Py_BuildValue("[]");
	if (out == NULL) {
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(aclflag2name); i++) {
		PyObject *val = NULL;

		if ((flags & aclflag2name[i].flag) == 0) {
			continue;
		}

		val = Py_BuildValue("s", aclflag2name[i].name);
		if (val == NULL) {
			Py_DECREF(out);
			return NULL;
		}

		err = PyList_Append(out, val);
		Py_XDECREF(val);
		if (err == -1) {
			Py_XDECREF(out);
			return NULL;
		}
	}

	return out;
}

static PyObject *permset_to_pylist(zfsace_permset_t perms)
{
	int i, err;
	PyObject *out = NULL;

	out = Py_BuildValue("[]");
	if (out == NULL) {
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(aceperm2name); i++) {
		PyObject *val = NULL;

		if ((perms & aceperm2name[i].perm) == 0) {
			continue;
		}

		val = Py_BuildValue("s", aceperm2name[i].name);
		if (val == NULL) {
			Py_DECREF(out);
			return NULL;
		}

		err = PyList_Append(out, val);
		Py_XDECREF(val);
		if (err == -1) {
			Py_XDECREF(out);
			return NULL;
		}
	}

	return out;
}

static PyObject *flagset_to_pylist(zfsace_flagset_t flags)
{
	int i, err;
	PyObject *out = NULL;
	out = Py_BuildValue("[]");
	if (out == NULL) {
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(aceflag2name); i++) {
		PyObject *val = NULL;

		if ((flags & aceflag2name[i].flag) == 0) {
			continue;
		}

		val = Py_BuildValue("s", aceflag2name[i].name);
		if (val == NULL) {
			Py_DECREF(out);
			return NULL;
		}

		err = PyList_Append(out, val);
		Py_XDECREF(val);
		if (err == -1) {
			Py_XDECREF(out);
			return NULL;
		}
	}

	return out;
}

static PyObject *whotype_to_pystring(zfsace_who_t whotype)
{
	int i;
	PyObject *out = NULL;

	for (i = 0; i < ARRAY_SIZE(acewho2name); i++) {
		if (whotype != acewho2name[i].who) {
			continue;
		}

		out = Py_BuildValue("s", acewho2name[i].name);
		if (out == NULL) {
			return NULL;
		}
		return out;
	}
	PyErr_Format(
		PyExc_ValueError,
		"%d is an invalid whotype", whotype
        );

	return NULL;
}

static PyObject *py_ace_new(PyTypeObject *obj,
			    PyObject *args_unused,
			    PyObject *kwargs_unused)
{
	py_acl_entry *self = NULL;

	self = (py_acl_entry *)obj->tp_alloc(obj, 0);
	if (self == NULL) {
		return NULL;
	}
	self->theace = NULL;
	self->parent_acl = NULL;
	return (PyObject *)self;
}

static int py_ace_init(PyObject *obj,
		       PyObject *args,
		       PyObject *kwargs)
{
	return 0;
}

static void py_ace_dealloc(py_acl_entry *self)
{
	if (self->parent_acl != NULL) {
		Py_CLEAR(self->parent_acl);
	}

	/*
	 * memory for ACL entry will be freed when
         * ACL is deallocated.
	 */
	self->theace = NULL;
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *permset_to_basic(zfsace_permset_t perms)
{
	PyObject *out = NULL;

	if (perms == ZFSACE_FULL_SET) {
		out = Py_BuildValue("s", "FULL_CONTROL");
		return out;
	}
	else if (perms == ZFSACE_MODIFY_SET) {
		out = Py_BuildValue("s", "MODIFY");
		return out;
	}
	else if (perms == (ZFSACE_READ_SET | ZFSACE_EXECUTE)) {
		out = Py_BuildValue("s", "READ");
		return out;
	}
	else if (perms == ZFSACE_TRAVERSE_SET) {
		out = Py_BuildValue("s", "TRAVERSE");
		return out;
	}

	Py_RETURN_NONE;
}

static PyObject *ace_get_permset(PyObject *obj, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;

	bool ok;
	zfsace_permset_t perms;
	PyObject *out = NULL;

	ok = zfsace_get_permset(self->theace, &perms);
	if (!ok) {
		set_exc_from_errno("zfsace_get_permset()");
		return NULL;
	}

	if (acl && acl->verbose) {
		PyObject *permlist = NULL;
		PyObject *basic = NULL;

		permlist = permset_to_pylist(perms);
		if (permlist == NULL) {
			return NULL;
		}

		basic = permset_to_basic(perms);
		if (basic == NULL) {
			Py_XDECREF(permlist);
			return NULL;
		}

		out = Py_BuildValue(
			"{s:I,s:O,s:O}",
			"raw", perms,
			"parsed", permlist,
			"basic", basic
		);
		Py_XDECREF(permlist);
		Py_XDECREF(basic);
	} else {
		out = Py_BuildValue("I", perms);
	}

	return out;
}

static bool parse_permset(py_acl *acl, PyObject *to_parse,
			  zfsace_permset_t *permset)
{
	unsigned long py_permset;

	if (!PyLong_Check(to_parse))
		return false;

	py_permset = PyLong_AsUnsignedLong(to_parse);

	if (py_permset == -1)
		return false;

	if (ZFSACE_ACCESS_MASK_INVALID(py_permset)) {
		PyErr_SetString(
			PyExc_ValueError,
			"invalid flagset."
		);
		return false;
	}

	*permset = (zfsace_permset_t)py_permset;
	return true;
}

static int ace_set_permset(PyObject *obj, PyObject *value, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;
	bool ok;
	zfsace_permset_t permset;

	ok = parse_permset(acl, value, &permset);
	if (!ok) {
		return -1;
	}

	ok = zfsace_set_permset(self->theace, permset);
	if (!ok) {
		set_exc_from_errno("zfsace_set_permset()");
		return -1;
	}
	return 0;
}

static PyObject *flagset_to_basic(zfsace_flagset_t flags)
{
	PyObject *out = NULL;

	/* inherited does not affect consideration of basic */
	flags &= ~ZFSACE_INHERITED_ACE;

	if (flags == (ZFSACE_DIRECTORY_INHERIT | ZFSACE_FILE_INHERIT)) {
		out = Py_BuildValue("s", "INHERIT");
		return out;
	}
	else if (flags == 0) {
		out = Py_BuildValue("s", "NO_INHERIT");
		return out;
	}

	Py_RETURN_NONE;
}

static PyObject *ace_get_flagset(PyObject *obj, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;
	bool ok;
	zfsace_flagset_t flags;
	PyObject *out = NULL;

	ok = zfsace_get_flagset(self->theace, &flags);
	if (!ok) {
		set_exc_from_errno("zfsace_get_flagset()");
		return NULL;
	}

	if (acl && acl->verbose) {
		PyObject *flaglist = NULL;
		PyObject *basic = NULL;
		flaglist = flagset_to_pylist(flags);
		if (flaglist == NULL) {
			return NULL;
		}

		basic = flagset_to_basic(flags);
		if (basic == NULL) {
			Py_XDECREF(flaglist);
			return NULL;
		}

		out = Py_BuildValue(
			"{s:I,s:O,s:O}",
			"raw", flags,
			"parsed", flaglist,
			"basic", basic
		);
		Py_XDECREF(flaglist);
		Py_XDECREF(basic);
	} else {
		out = Py_BuildValue("I", flags);
	}

	return out;
}

static bool parse_flagset(py_acl *acl, PyObject *to_parse,
			  zfsace_flagset_t *flagset)
{
	unsigned long py_flagset;

	if (!PyLong_Check(to_parse))
		return false;

	py_flagset = PyLong_AsUnsignedLong(to_parse);

	if (py_flagset == -1)
		return false;

	if (ZFSACE_FLAG_INVALID(py_flagset)) {
		PyErr_SetString(
			PyExc_ValueError,
			"invalid flagset."
		);
		return false;
	}

	*flagset = (zfsace_flagset_t)py_flagset;
	return true;
}

static int ace_set_flagset(PyObject *obj, PyObject *value, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;
	bool ok;
	zfsace_flagset_t flagset;

	ok = parse_flagset(acl, value, &flagset);
	if (!ok) {
		return -1;
	}

	ok = zfsace_set_flagset(self->theace, flagset);
	if (!ok) {
		set_exc_from_errno("zfsace_set_flagset()");
		return -1;
	}
	return 0;
}

static PyObject *verbose_who(zfsace_who_t whotype, zfsace_id_t whoid)
{
	PyObject *pywhotype = NULL;
	PyObject *pywhoid = NULL;
	PyObject *verbose_whotype = NULL;
	PyObject *out = NULL;

	pywhotype = whotype_to_pystring(whotype);
	if (pywhotype == NULL) {
		return NULL;
	}

	verbose_whotype = Py_BuildValue(
		"{s:I,s:O}",
		"raw", whotype,
		"parsed", pywhotype
	);

	Py_XDECREF(pywhotype);

	/*
	 * In future it may make sense to add getpwuid_r / getgrgid_r call here
	 */
	pywhoid = Py_BuildValue(
		"{s:I,s:I}",
		"raw", whoid,
		"parsed", whoid
	);

	if (pywhoid == NULL) {
		Py_XDECREF(verbose_whotype);
		return NULL;
	}

	out = Py_BuildValue(
		"{s:O,s:O}",
		"who_type", verbose_whotype,
		"who_id", pywhoid);

	Py_XDECREF(verbose_whotype);
	Py_XDECREF(pywhoid);
	return out;
}

static PyObject *ace_get_who(PyObject *obj, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;
	bool ok;
	zfsace_who_t whotype;
	zfsace_id_t whoid;
	PyObject *out = NULL;

	ok = zfsace_get_who(self->theace, &whotype, &whoid);
	if (!ok) {
		set_exc_from_errno("zfsace_get_who()");
		return NULL;
	}

	if (acl && acl->verbose) {
		out = verbose_who(whotype, whoid);
	} else {
		out = Py_BuildValue("II", whotype, whoid);
	}
	return out;
}

static bool parse_who(py_acl *acl, PyObject *to_parse,
		      zfsace_who_t *whotype, zfsace_id_t *whoid)
{
	int pywhotype, pywhoid;

	if (!PyArg_ParseTuple(to_parse, "ii", &pywhotype, &pywhoid))
		return false;

	if (SPECIAL_WHO_INVALID(pywhotype)) {
		PyErr_SetString(
			PyExc_ValueError,
			"invalid whotype."
		);
		return false;
	}

	if ((pywhoid < 0) && (pywhoid != -1)) {
		PyErr_SetString(
			PyExc_ValueError,
			"invalid id"
		);
		return false;
	}

	if ((pywhoid == -1) &&
	    ((pywhotype == ZFSACL_USER) || (pywhotype == ZFSACL_USER))) {
		PyErr_SetString(
			PyExc_ValueError,
			"-1 is invalid ID for named entries."
		);
		return false;
	}

	if (pywhoid > INT32_MAX) {
		PyErr_SetString(
			PyExc_ValueError,
			"ID for named entry is too large."
		);
		return false;
	}

	*whotype = (zfsace_who_t)pywhotype;
	*whoid = (zfsace_id_t)pywhoid;

	return true;
}

static int ace_set_who(PyObject *obj, PyObject *value, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;
	zfsace_who_t whotype;
	zfsace_id_t whoid;
	bool ok;

	ok = parse_who(acl, value, &whotype, &whoid);
	if (!ok) {
		return -1;
	}

	ok = zfsace_set_who(self->theace, whotype, whoid);
	if (!ok) {
		set_exc_from_errno("zfsace_set_who()");
		return -1;
	}
	return 0;
}

static PyObject *ace_get_entry_type(PyObject *obj, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;
	bool ok;
	zfsace_entry_type_t entry_type;
	PyObject *out = NULL;

	ok = zfsace_get_entry_type(self->theace, &entry_type);
	if (!ok) {
		set_exc_from_errno("zfsace_get_entry_type()");
		return NULL;
	}

	if (acl && acl->verbose) {
		const char *entry_str = NULL;

		switch(entry_type) {
		case ZFSACL_ENTRY_TYPE_ALLOW:
			entry_str = "ALLOW";
			break;
		case ZFSACL_ENTRY_TYPE_DENY:
			entry_str = "DENY";
			break;
		default:
			PyErr_Format(
				PyExc_ValueError,
				"%d is an invalid entry type",
				entry_type
			);
			return NULL;
		}
		out = Py_BuildValue(
			"{s:I,s:s}",
			"raw", entry_type,
			"parsed", entry_str
		);
	} else {
		out = Py_BuildValue("I", entry_type);
	}
	return out;
}

static bool parse_entry_type(py_acl *acl, PyObject *to_parse,
			     zfsace_entry_type_t *entry_type)
{
	unsigned long py_entry_type;


	if (!PyLong_Check(to_parse))
		return false;
	py_entry_type = PyLong_AsUnsignedLong(to_parse);

	if (py_entry_type == -1)
		return false;

	if (ZFSACE_TYPE_INVALID(py_entry_type)) {
		PyErr_SetString(
			PyExc_ValueError,
			"invalid ACL entry type."
		);
		return false;
	}

	*entry_type = (zfsace_entry_type_t)py_entry_type;
	return true;
}

static int ace_set_entry_type(PyObject *obj, PyObject *value, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
        py_acl *acl = self->parent_acl;
	bool ok;
	zfsace_entry_type_t entry_type;

	ok = parse_entry_type(acl, value, &entry_type);
	if (!ok) {
		return -1;
	}

	ok = zfsace_set_entry_type(self->theace, entry_type);
	if (!ok) {
		set_exc_from_errno("zfsace_set_entry_type()");
		return -1;
	}

	return 0;
}

static PyObject *ace_get_idx(PyObject *obj, void *closure)
{
	py_acl_entry *self = (py_acl_entry *)obj;
	return Py_BuildValue("i", self->idx);
}

static PyMethodDef ace_object_methods[] = {
	{ NULL, NULL, 0, NULL }
};

static PyGetSetDef ace_object_getsetters[] = {
	{
		.name    = discard_const_p(char, "idx"),
		.get     = (getter)ace_get_idx,
	},
	{
		.name    = discard_const_p(char, "permset"),
		.get     = (getter)ace_get_permset,
		.set     = (setter)ace_set_permset,
	},
	{
		.name    = discard_const_p(char, "flagset"),
		.get     = (getter)ace_get_flagset,
		.set     = (setter)ace_set_flagset,
	},
	{
		.name    = discard_const_p(char, "who"),
		.get     = (getter)ace_get_who,
		.set     = (setter)ace_set_who,
	},
	{
		.name    = discard_const_p(char, "entry_type"),
		.get     = (getter)ace_get_entry_type,
		.set     = (setter)ace_set_entry_type,
	},
	{ .name = NULL }
};

static PyTypeObject PyZfsACLEntry = {
	.tp_name = "zfsacl.ACLEntry",
	.tp_basicsize = sizeof(py_acl_entry),
	.tp_methods = ace_object_methods,
	.tp_getset = ace_object_getsetters,
	.tp_new = py_ace_new,
	.tp_init = py_ace_init,
	.tp_doc = "An ACL Entry",
	.tp_dealloc = (destructor)py_ace_dealloc,
	.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,
};

static PyObject *py_acl_new(PyTypeObject *obj,
			    PyObject *args_unused,
			    PyObject *kwargs_unused)
{
	py_acl *self = NULL;

	self = (py_acl *)obj->tp_alloc(obj, 0);
	if (self == NULL) {
		return NULL;
	}
	self->theacl = NULL;
	self->verbose = false;
	return (PyObject *)self;
}

static int py_acl_init(PyObject *obj,
		       PyObject *args,
		       PyObject *kwargs)
{
	py_acl *self = (py_acl *)obj;
	zfsacl_t theacl = NULL;
	const char *kwnames [] = { "fd", "path", "brand", NULL };
	int fd = 0, brand = ZFSACL_BRAND_NFSV4;
	char *path = NULL;
	char *aclbrand = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|isi",
					 kwnames, &fd, &path, &brand)) {
		return -1;
	}

	if (fd != 0) {
		theacl = zfsacl_get_fd(fd, brand);
		if (theacl == NULL) {
			set_exc_from_errno("zfsacl_get_fd()");
			return -1;
		}
	}
	else if (path != NULL) {
		theacl = zfsacl_get_file(path, brand);
		if (theacl == NULL) {
			set_exc_from_errno("zfsacl_get_file()");
			return -1;
		}
	} else {
		theacl = zfsacl_init(ZFSACL_MAX_ENTRIES, brand);
		if (theacl == NULL) {
			set_exc_from_errno("zfsacl_get_file()");
			return -1;
		}
	}

	if (theacl == NULL) {
		set_exc_from_errno("zfsace_set_entry_type()");
		return -1;
	}

	self->theacl = theacl;

	return 0;
}

static void py_acl_dealloc(py_acl *self)
{
	if (self->theacl != NULL) {
		zfsacl_free(&self->theacl);
		self->theacl = NULL;
	}
	Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *acl_get_verbose(PyObject *obj, void *closure)
{
	py_acl *self = (py_acl *)obj;

	if (self->verbose) {
		Py_RETURN_TRUE;
	}
	Py_RETURN_FALSE;
}

static int acl_set_verbose(PyObject *obj, PyObject *value, void *closure)
{
	py_acl *self = (py_acl *)obj;

	if (!PyBool_Check(value)) {
		PyErr_SetString(
			PyExc_TypeError,
			"value must be boolean."
		);
		return -1;
	}

	self->verbose = (value == Py_True) ? true : false;
	return 0;
}

static PyObject *acl_get_flags(PyObject *obj, void *closure)
{
	py_acl *self = (py_acl *)obj;
	bool ok;
	zfsacl_aclflags_t flags;
	PyObject *out = NULL;

	ok = zfsacl_get_aclflags(self->theacl, &flags);
	if (!ok) {
		set_exc_from_errno("zfsacl_get_aclflags()");
		return NULL;
	}

	out = Py_BuildValue("I", flags);
	return out;
}

static int acl_set_flags(PyObject *obj, PyObject *value, void *closure)
{
	py_acl *self = (py_acl *)obj;
	long val;
	zfsacl_aclflags_t flags;
	bool ok;

	if (!PyLong_Check(value)) {
		PyErr_SetString(
			PyExc_TypeError,
			"flags must be integer"
		);
		return -1;
	}

	val = PyLong_AsLong(value);

	if (ZFSACL_FLAGS_INVALID(val)) {
		PyErr_SetString(
			PyExc_ValueError,
			"Invalid ACL flags specified"
		);
		return -1;
	}

	ok = zfsacl_set_aclflags(self->theacl, (zfsacl_aclflags_t)val);
	if (!ok) {
		set_exc_from_errno("zfsacl_set_aclflags()");
		return -1;
	}

	return 0;
}

static PyObject *acl_get_brand(PyObject *obj, void *closure)
{
	py_acl *self = (py_acl *)obj;
	bool ok;
	zfsacl_brand_t brand;
	PyObject *out = NULL;

	ok = zfsacl_get_brand(self->theacl, &brand);
	if (!ok) {
		set_exc_from_errno("zfsacl_get_brand()");
		return NULL;
	}

	out = Py_BuildValue("I", brand);
	return out;
}

static PyObject *acl_get_acecnt(PyObject *obj, void *closure)
{
	py_acl *self = (py_acl *)obj;
	bool ok;
	uint acecnt;
	PyObject *out = NULL;

	ok = zfsacl_get_acecnt(self->theacl, &acecnt);
	if (!ok) {
		set_exc_from_errno("zfsacl_get_acecnt()");
		return NULL;
	}

	out = Py_BuildValue("I", acecnt);
	return out;
}

static bool initialize_py_ace(py_acl *self,
			      PyObject *in,
			      int idx,
			      zfsacl_entry_t entry)
{
	py_acl_entry *out = (py_acl_entry *)in;
	bool ok;
	uint acecnt;

	ok = zfsacl_get_acecnt(self->theacl, &acecnt);
	if (!ok) {
		set_exc_from_errno("zfsacl_get_acecnt()");
		return false;
	}

	out->theace = entry;
	out->parent_acl = self;
	out->initial_cnt = acecnt;
	out->idx = (idx == ZFSACL_APPEND_ENTRY) ? (int)acecnt : idx;
	Py_INCREF(out->parent_acl);
	return true;
}

static bool pyargs_get_index(py_acl *self, PyObject *args, int *pidx, bool required)
{
	int val = -1;
	bool ok;
	uint acecnt;
	const char *format = required ? "i" : "|i";

	if (!PyArg_ParseTuple(args, format, &val))
		return false;

	if (val == -1) {
		*pidx = ZFSACL_APPEND_ENTRY;
		return true;
	}
	else if (val == 0) {
		*pidx = 0;
		return true;
	}

	if (val < 0) {
		PyErr_SetString(
			PyExc_ValueError,
			"Index may not be negative"
		);
		return false;
	}

	if (val > (ZFSACL_MAX_ENTRIES -1)) {
		PyErr_SetString(
			PyExc_ValueError,
			"Index exceeds maximum entries for ACL"
		);
		return false;
	}

	ok = zfsacl_get_acecnt(self->theacl, &acecnt);
	if (!ok) {
		set_exc_from_errno("zfsacl_get_acecnt()");
		return false;
	}

	if ((acecnt == 0) || (val > acecnt -1)) {
		PyErr_Format(
			PyExc_IndexError,
			"%ld: index invalid, ACL contains (%u) entries.",
			val, acecnt
		);
		return false;
	}

	if (val > (ZFSACL_MAX_ENTRIES -1)) {
		PyErr_SetString(
			PyExc_ValueError,
			"Index exceeds maximum entries for ACL"
		);
		return false;
	}

	*pidx = val;
	return true;
}

static PyObject *py_acl_create_entry(PyObject *obj, PyObject *args)
{
	py_acl *self = (py_acl *)obj;
	bool ok;
	int idx;
	zfsacl_entry_t entry = NULL;
	PyObject *pyentry = NULL;

	ok = pyargs_get_index(self, args, &idx, false);
	if (!ok) {
		return NULL;
	}

	ok = zfsacl_create_aclentry(self->theacl, idx, &entry);
	if (!ok) {
		set_exc_from_errno("zfsacl_create_aclentry()");
		return NULL;
	}

	pyentry = PyObject_CallFunction((PyObject *)&PyZfsACLEntry, NULL);
	ok = initialize_py_ace(self, pyentry, idx, entry);
	if (!ok) {
		Py_CLEAR(pyentry);
		return NULL;
	}

	return (PyObject *)pyentry;
}

static PyObject *py_acl_get_entry(PyObject *obj, PyObject *args)
{
	py_acl *self = (py_acl *)obj;
	bool ok;
	int idx;
	zfsacl_entry_t entry = NULL;
	PyObject *pyentry = NULL;

	ok = pyargs_get_index(self, args, &idx, true);
	if (!ok) {
		return NULL;
	}

	ok = zfsacl_get_aclentry(self->theacl, idx, &entry);
	if (!ok) {
		set_exc_from_errno("zfsacl_get_aclentry()");
		return NULL;
	}

	pyentry = PyObject_CallFunction((PyObject *)&PyZfsACLEntry, NULL);
	ok = initialize_py_ace(self, pyentry, idx, entry);
	if (!ok) {
		Py_CLEAR(pyentry);
		return NULL;
	}

	return (PyObject *)pyentry;
}

static PyObject *py_acl_delete_entry(PyObject *obj, PyObject *args)
{
	py_acl *self = (py_acl *)obj;
	bool ok;
	int idx;

	ok = pyargs_get_index(self, args, &idx, true);
	if (!ok) {
		return NULL;
	}

	ok = zfsacl_delete_aclentry(self->theacl, idx);
	if (!ok) {
		if ((errno == ERANGE) && (idx == 0)) {
			PyErr_SetString(
				PyExc_ValueError,
				"At least one ACL entry is required."
			);
			return NULL;
		}
		set_exc_from_errno("zfsacl_delete_aclentry()");
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyObject *py_acl_set(PyObject *obj, PyObject *args, PyObject *kwargs)
{
	py_acl *self = (py_acl *)obj;
	bool ok;
	int fd = -1;
	const char *path = NULL;
	const char *kwnames [] = { "fd", "path", NULL };

	ok = PyArg_ParseTupleAndKeywords(
		args, kwargs, "|is", kwnames, &fd, &path
	);

	if (!ok) {
		return NULL;
	}

	if (fd != -1) {
		ok = zfsacl_set_fd(fd, self->theacl);
		if (!ok) {
			set_exc_from_errno("zfsacl_set_fd()");
			return NULL;
		}
	}
	else if (path != NULL) {
		ok = zfsacl_set_file(path, self->theacl);
		if (!ok) {
			set_exc_from_errno("zfsacl_set_file()");
			return NULL;
		}
	} else {
		PyErr_SetString(
			PyExc_ValueError,
			"`fd` or `path` key is required"
		);
	}

	Py_RETURN_NONE;
}

static PyObject *py_acl_iter(PyObject *obj, PyObject *args_unused)
{
	py_acl *self = (py_acl *)obj;
	py_acl_iterator *out = NULL;

	out = PyObject_New(py_acl_iterator, &PyACLIterator);
	if (out == NULL) {
		return NULL;
	}

	out->current_idx = 0;
	out->acl = self;
	Py_INCREF(self);
	return (PyObject *)out;
}

static PyObject *py_native_data(PyObject *obj, PyObject *args, PyObject *kwargs)
{
	py_acl *self = (py_acl *)obj;
	PyObject *out =NULL;
	struct native_acl native;
	bool ok;

	ok = zfsacl_to_native(self->theacl, &native);
	if (!ok) {
		set_exc_from_errno("zfsacl_to_native()");
		return NULL;
	}

	out = PyBytes_FromStringAndSize(
		(const char *)native.data,
		native.datalen
	);
	free(native.data);
	return out;
}

static PyMethodDef acl_object_methods[] = {
	{
		.ml_name = "setacl",
		.ml_meth = (PyCFunction)py_acl_set,
		.ml_flags = METH_VARARGS|METH_KEYWORDS,
		.ml_doc = "Set ACL on path or fd"
	},
	{
		.ml_name = "create_entry",
		.ml_meth = py_acl_create_entry,
		.ml_flags = METH_VARARGS,
		.ml_doc = "Create new entry at specied index"
	},
	{
		.ml_name = "get_entry",
		.ml_meth = py_acl_get_entry,
		.ml_flags = METH_VARARGS,
		.ml_doc = "Get entry at specied index"
	},
	{
		.ml_name = "delete_entry",
		.ml_meth = py_acl_delete_entry,
		.ml_flags = METH_VARARGS,
		.ml_doc = "Delete entry by specied index"
	},
	{
		.ml_meth = (PyCFunction)py_native_data,
		.ml_flags = METH_VARARGS|METH_KEYWORDS,
		.ml_doc = "Returns bytes of native ACL"
	},
	{ NULL, NULL, 0, NULL }
};

static PyGetSetDef acl_object_getsetters[] = {
	{
		.name    = discard_const_p(char, "verbose_output"),
		.get     = (getter)acl_get_verbose,
		.set     = (setter)acl_set_verbose,
	},
	{
		.name    = discard_const_p(char, "acl_flags"),
		.get     = (getter)acl_get_flags,
		.set     = (setter)acl_set_flags,
	},
	{
		.name    = discard_const_p(char, "brand"),
		.get     = (getter)acl_get_brand,
	},
	{
		.name    = discard_const_p(char, "ace_count"),
		.get     = (getter)acl_get_acecnt,
	},
	{ .name = NULL }
};

static PyTypeObject PyZfsACL = {
	.tp_name = "zfsacl.ACL",
	.tp_basicsize = sizeof(py_acl),
	.tp_methods = acl_object_methods,
	.tp_getset = acl_object_getsetters,
	.tp_new = py_acl_new,
	.tp_init = py_acl_init,
	.tp_doc = "An ACL",
	.tp_dealloc = (destructor)py_acl_dealloc,
	.tp_iter = (getiterfunc)py_acl_iter,
	.tp_flags = Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE|Py_TPFLAGS_HAVE_ITER,
};

static PyMethodDef acl_module_methods[] = {
	{ .ml_name = NULL }
};
#define MODULE_DOC "ZFS ACL python bindings."

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	.m_name = "zfsacl",
	.m_doc = MODULE_DOC,
	.m_size = -1,
	.m_methods = acl_module_methods,
};

PyObject* module_init(void);
PyObject* module_init(void)
{
	PyObject *m = NULL;
	m = PyModule_Create(&moduledef);
	if (m == NULL) {
		fprintf(stderr, "failed to initalize module\n");
		return NULL;
	}

	if (PyType_Ready(&PyZfsACL) < 0)
		return NULL;

	if (PyType_Ready(&PyZfsACLEntry) < 0)
		return NULL;

	/* ZFS ACL branding */
	PyModule_AddIntConstant(m, "BRAND_UNKNOWN", ZFSACL_BRAND_UNKNOWN);
	PyModule_AddIntConstant(m, "BRAND_ACCESS", ZFSACL_BRAND_ACCESS);
	PyModule_AddIntConstant(m, "BRAND_DEFAULT", ZFSACL_BRAND_DEFAULT);
	PyModule_AddIntConstant(m, "BRAND_NFSV4", ZFSACL_BRAND_NFSV4);

	/* ZFS ACL whotypes */
	PyModule_AddIntConstant(m, "WHOTYPE_UNDEFINED", ZFSACL_UNDEFINED_TAG);
	PyModule_AddIntConstant(m, "WHOTYPE_USER_OBJ", ZFSACL_USER_OBJ);
	PyModule_AddIntConstant(m, "WHOTYPE_GROUP_OBJ", ZFSACL_GROUP_OBJ);
	PyModule_AddIntConstant(m, "WHOTYPE_EVERYONE", ZFSACL_EVERYONE);
	PyModule_AddIntConstant(m, "WHOTYPE_USER", ZFSACL_USER);
	PyModule_AddIntConstant(m, "WHOTYPE_GROUP", ZFSACL_GROUP);
	PyModule_AddIntConstant(m, "WHOTYPE_MASK", ZFSACL_MASK);

	/* ZFS ACL entry types */
	PyModule_AddIntConstant(m, "ENTRY_TYPE_ALLOW", ZFSACL_ENTRY_TYPE_ALLOW);
	PyModule_AddIntConstant(m, "ENTRY_TYPE_DENY", ZFSACL_ENTRY_TYPE_DENY);

	/* ZFS ACL ACL-wide flags */
	PyModule_AddIntConstant(m, "ACL_AUTO_INHERIT", ZFSACL_AUTO_INHERIT);
	PyModule_AddIntConstant(m, "ACL_PROTECTED", ZFSACL_PROTECTED);
	PyModule_AddIntConstant(m, "ACL_DEFAULT", ZFSACL_DEFAULTED);

	/* valid on get, but not set */
	PyModule_AddIntConstant(m, "ACL_IS_TRIVIAL", ZFSACL_IS_TRIVIAL);

	/* ZFS ACL inherit flags (NFSv4 only) */
	PyModule_AddIntConstant(m, "FLAG_FILE_INHERIT", ZFSACE_FILE_INHERIT);
	PyModule_AddIntConstant(m, "FLAG_DIRECTORY_INHERIT", ZFSACE_DIRECTORY_INHERIT);
	PyModule_AddIntConstant(m, "FLAG_NO_PROPAGATE_INHERIT", ZFSACE_NO_PROPAGATE_INHERIT);
	PyModule_AddIntConstant(m, "FLAG_INHERIT_ONLY", ZFSACE_INHERIT_ONLY);
	PyModule_AddIntConstant(m, "FLAG_INHERITED", ZFSACE_INHERITED_ACE);

	/* ZFS ACL permissions */
	/* POSIX1e and NFSv4 */
	PyModule_AddIntConstant(m, "PERM_READ_DATA", ZFSACE_READ_DATA);
	PyModule_AddIntConstant(m, "PERM_WRITE_DATA", ZFSACE_WRITE_DATA);
	PyModule_AddIntConstant(m, "PERM_EXECUTE", ZFSACE_EXECUTE);

	/* NFSv4 only */
	PyModule_AddIntConstant(m, "PERM_LIST_DIRECTORY", ZFSACE_LIST_DIRECTORY);
	PyModule_AddIntConstant(m, "PERM_ADD_FILE", ZFSACE_ADD_FILE);
	PyModule_AddIntConstant(m, "PERM_APPEND_DATA", ZFSACE_APPEND_DATA);
	PyModule_AddIntConstant(m, "PERM_ADD_SUBDIRECTORY", ZFSACE_ADD_SUBDIRECTORY);
	PyModule_AddIntConstant(m, "PERM_READ_NAMED_ATTRS", ZFSACE_READ_NAMED_ATTRS);
	PyModule_AddIntConstant(m, "PERM_WRITE_NAMED_ATTRS", ZFSACE_WRITE_NAMED_ATTRS);
	PyModule_AddIntConstant(m, "PERM_DELETE_CHILD", ZFSACE_DELETE_CHILD);
	PyModule_AddIntConstant(m, "PERM_READ_ATTRIBUTES", ZFSACE_READ_ATTRIBUTES);
	PyModule_AddIntConstant(m, "PERM_WRITE_ATTRIBUTES", ZFSACE_WRITE_ATTRIBUTES);
	PyModule_AddIntConstant(m, "PERM_DELETE", ZFSACE_DELETE);
	PyModule_AddIntConstant(m, "PERM_READ_ACL", ZFSACE_READ_ACL);
	PyModule_AddIntConstant(m, "PERM_WRITE_ACL", ZFSACE_WRITE_ACL);
	PyModule_AddIntConstant(m, "PERM_WRITE_OWNER", ZFSACE_WRITE_OWNER);
	PyModule_AddIntConstant(m, "PERM_SYNCHRONIZE", ZFSACE_SYNCHRONIZE);
	PyModule_AddIntConstant(m, "BASIC_PERM_FULL_CONTROL", ZFSACE_FULL_SET);
	PyModule_AddIntConstant(m, "BASIC_PERM_MODIFY", ZFSACE_MODIFY_SET);
	PyModule_AddIntConstant(m, "BASIC_PERM_READ", ZFSACE_READ_SET | ZFSACE_EXECUTE);
	PyModule_AddIntConstant(m, "BASIC_PERM_TRAVERSE", ZFSACE_TRAVERSE_SET);

	PyModule_AddObject(m, "Acl", (PyObject *)&PyZfsACL);

	return m;
}

PyMODINIT_FUNC PyInit_zfsacl(void);
PyMODINIT_FUNC PyInit_zfsacl(void)
{
	return module_init();
}
