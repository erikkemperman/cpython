// types.IntersectionType -- used to represent e.g. Intersection[int, str], int & str
#include "Python.h"
#include "pycore_intersectionobject.h"
#include "pycore_object.h"         // _PyObject_GC_TRACK/UNTRACK
#include "pycore_typevarobject.h"  // _PyTypeAlias_Type
#include "pycore_unicodeobject.h" // _PyUnicode_EqualToASCIIString
#include "pycore_unionobject.h"    // _Py_union_type_or, _PyUnion_Check


typedef struct {
    PyObject_HEAD
    PyObject *args;  // all args (tuple)
    PyObject *hashable_args;  // frozenset or NULL
    PyObject *unhashable_args;  // tuple or NULL
    PyObject *parameters;
    PyObject *weakreflist;
} intersectionobject;

static void
intersectionobject_dealloc(PyObject *self)
{
    intersectionobject *alias = (intersectionobject *)self;

    _PyObject_GC_UNTRACK(self);
    if (alias->weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject *)alias);
    }

    Py_XDECREF(alias->args);
    Py_XDECREF(alias->hashable_args);
    Py_XDECREF(alias->unhashable_args);
    Py_XDECREF(alias->parameters);
    Py_TYPE(self)->tp_free(self);
}

static int
intersection_traverse(PyObject *self, visitproc visit, void *arg)
{
    intersectionobject *alias = (intersectionobject *)self;
    Py_VISIT(alias->args);
    Py_VISIT(alias->hashable_args);
    Py_VISIT(alias->unhashable_args);
    Py_VISIT(alias->parameters);
    return 0;
}

static Py_hash_t
intersection_hash(PyObject *self)
{
    intersectionobject *alias = (intersectionobject *)self;
    // If there are any unhashable args, treat this intersection as unhashable.
    // Otherwise, two intersections might compare equal but have different hashes.
    if (alias->unhashable_args) {
        // Attempt to get an error from one of the values.
        assert(PyTuple_CheckExact(alias->unhashable_args));
        Py_ssize_t n = PyTuple_GET_SIZE(alias->unhashable_args);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *arg = PyTuple_GET_ITEM(alias->unhashable_args, i);
            Py_hash_t hash = PyObject_Hash(arg);
            if (hash == -1) {
                return -1;
            }
        }
        // The unhashable values somehow became hashable again. Still raise
        // an error.
        PyErr_Format(PyExc_TypeError, "intersection contains %d unhashable elements", n);
        return -1;
    }
    return PyObject_Hash(alias->hashable_args);
}

static int
intersections_equal(intersectionobject *a, intersectionobject *b)
{
    int result = PyObject_RichCompareBool(a->hashable_args, b->hashable_args, Py_EQ);
    if (result == -1) {
        return -1;
    }
    if (result == 0) {
        return 0;
    }
    if (a->unhashable_args && b->unhashable_args) {
        Py_ssize_t n = PyTuple_GET_SIZE(a->unhashable_args);
        if (n != PyTuple_GET_SIZE(b->unhashable_args)) {
            return 0;
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *arg_a = PyTuple_GET_ITEM(a->unhashable_args, i);
            int result = PySequence_Contains(b->unhashable_args, arg_a);
            if (result == -1) {
                return -1;
            }
            if (!result) {
                return 0;
            }
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *arg_b = PyTuple_GET_ITEM(b->unhashable_args, i);
            int result = PySequence_Contains(a->unhashable_args, arg_b);
            if (result == -1) {
                return -1;
            }
            if (!result) {
                return 0;
            }
        }
    }
    else if (a->unhashable_args || b->unhashable_args) {
        return 0;
    }
    return 1;
}

static PyObject *
intersection_richcompare(PyObject *a, PyObject *b, int op)
{
    if (!_PyIntersection_Check(b) || (op != Py_EQ && op != Py_NE)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    int equal = intersections_equal((intersectionobject*)a, (intersectionobject*)b);
    if (equal == -1) {
        return NULL;
    }
    if (op == Py_EQ) {
        return PyBool_FromLong(equal);
    }
    else {
        return PyBool_FromLong(!equal);
    }
}

typedef struct {
    PyObject *args;  // list
    PyObject *hashable_args;  // set
    PyObject *unhashable_args;  // list or NULL
    bool is_checked;  // whether to call type_check()
} intersectionbuilder;

static bool intersectionbuilder_add_tuple(intersectionbuilder *, PyObject *);
static PyObject *make_intersection(intersectionbuilder *);
static PyObject *type_check(PyObject *, const char *);

static bool
intersectionbuilder_init(intersectionbuilder *ub, bool is_checked)
{
    ub->args = PyList_New(0);
    if (ub->args == NULL) {
        return false;
    }
    ub->hashable_args = PySet_New(NULL);
    if (ub->hashable_args == NULL) {
        Py_DECREF(ub->args);
        return false;
    }
    ub->unhashable_args = NULL;
    ub->is_checked = is_checked;
    return true;
}

static void
intersectionbuilder_finalize(intersectionbuilder *ub)
{
    Py_DECREF(ub->args);
    Py_DECREF(ub->hashable_args);
    Py_XDECREF(ub->unhashable_args);
}

static bool
intersectionbuilder_add_single_unchecked(intersectionbuilder *ub, PyObject *arg)
{
    Py_hash_t hash = PyObject_Hash(arg);
    if (hash == -1) {
        PyErr_Clear();
        if (ub->unhashable_args == NULL) {
            ub->unhashable_args = PyList_New(0);
            if (ub->unhashable_args == NULL) {
                return false;
            }
        }
        else {
            int contains = PySequence_Contains(ub->unhashable_args, arg);
            if (contains < 0) {
                return false;
            }
            if (contains == 1) {
                return true;
            }
        }
        if (PyList_Append(ub->unhashable_args, arg) < 0) {
            return false;
        }
    }
    else {
        int contains = PySet_Contains(ub->hashable_args, arg);
        if (contains < 0) {
            return false;
        }
        if (contains == 1) {
            return true;
        }
        if (PySet_Add(ub->hashable_args, arg) < 0) {
            return false;
        }
    }
    return PyList_Append(ub->args, arg) == 0;
}

static bool
intersectionbuilder_add_single(intersectionbuilder *ub, PyObject *arg)
{
    if (Py_IsNone(arg)) {
        arg = (PyObject *)&_PyNone_Type;  // immortal, so no refcounting needed
    }
    else if (_PyIntersection_Check(arg)) {
        PyObject *args = ((intersectionobject *)arg)->args;
        return intersectionbuilder_add_tuple(ub, args);
    }
    if (ub->is_checked) {
        PyObject *type = type_check(arg, "Intersection[arg, ...]: each arg must be a type.");
        if (type == NULL) {
            return false;
        }
        bool result = intersectionbuilder_add_single_unchecked(ub, type);
        Py_DECREF(type);
        return result;
    }
    else {
        return intersectionbuilder_add_single_unchecked(ub, arg);
    }
}

static bool
intersectionbuilder_add_tuple(intersectionbuilder *ub, PyObject *tuple)
{
    Py_ssize_t n = PyTuple_GET_SIZE(tuple);
    for (Py_ssize_t i = 0; i < n; i++) {
        if (!intersectionbuilder_add_single(ub, PyTuple_GET_ITEM(tuple, i))) {
            return false;
        }
    }
    return true;
}

static int
is_intersectionable(PyObject *obj)
{
    if (obj == Py_None ||
        PyType_Check(obj) ||
        _PyGenericAlias_Check(obj) ||
        _PyIntersection_Check(obj) ||
        _PyIntersection_Check(obj) ||
        Py_IS_TYPE(obj, &_PyTypeAlias_Type)) {
        return 1;
    }
    return 0;
}

PyObject *
_Py_intersection_type_and(PyObject* self, PyObject* other)
{
    if (!is_intersectionable(self) || !is_intersectionable(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    intersectionbuilder ub;
    // unchecked because we already checked is_intersectionable()
    if (!intersectionbuilder_init(&ub, false)) {
        return NULL;
    }
    if (!intersectionbuilder_add_single(&ub, self) ||
        !intersectionbuilder_add_single(&ub, other)) {
        intersectionbuilder_finalize(&ub);
        return NULL;
    }

    PyObject *new_intersection = make_intersection(&ub);
    return new_intersection;
}

static PyObject *
intersection_repr(PyObject *self)
{
    intersectionobject *alias = (intersectionobject *)self;
    Py_ssize_t len = PyTuple_GET_SIZE(alias->args);

    // Shortest type name "int" (3 chars) + " & " (3 chars) separator
    Py_ssize_t estimate = (len <= PY_SSIZE_T_MAX / 6) ? len * 6 : len;
    PyUnicodeWriter *writer = PyUnicodeWriter_Create(estimate);
    if (writer == NULL) {
        return NULL;
    }

    for (Py_ssize_t i = 0; i < len; i++) {
        if (i > 0 && PyUnicodeWriter_WriteASCII(writer, " & ", 3) < 0) {
            goto error;
        }
        PyObject *p = PyTuple_GET_ITEM(alias->args, i);
        if (_Py_typing_type_repr(writer, p) < 0) {
            goto error;
        }
    }

#if 0
    PyUnicodeWriter_WriteASCII(writer, "|args=", 6);
    PyUnicodeWriter_WriteRepr(writer, alias->args);
    PyUnicodeWriter_WriteASCII(writer, "|h=", 3);
    PyUnicodeWriter_WriteRepr(writer, alias->hashable_args);
    if (alias->unhashable_args) {
        PyUnicodeWriter_WriteASCII(writer, "|u=", 3);
        PyUnicodeWriter_WriteRepr(writer, alias->unhashable_args);
    }
#endif

    return PyUnicodeWriter_Finish(writer);

error:
    PyUnicodeWriter_Discard(writer);
    return NULL;
}

static PyMemberDef intersection_members[] = {
        {"__args__", _Py_T_OBJECT, offsetof(intersectionobject, args), Py_READONLY},
        {0}
};

// Populate __parameters__ if needed.
static int
intersection_init_parameters(intersectionobject *alias)
{
    int result = 0;
    Py_BEGIN_CRITICAL_SECTION(alias);
    if (alias->parameters == NULL) {
        alias->parameters = _Py_make_parameters(alias->args);
        if (alias->parameters == NULL) {
            result = -1;
        }
    }
    Py_END_CRITICAL_SECTION();
    return result;
}

static PyObject *
intersection_getitem(PyObject *self, PyObject *item)
{
    intersectionobject *alias = (intersectionobject *)self;
    if (intersection_init_parameters(alias) < 0) {
        return NULL;
    }

    PyObject *newargs = _Py_subs_parameters(self, alias->args, alias->parameters, item);
    if (newargs == NULL) {
        return NULL;
    }

    PyObject *res = _Py_intersection_from_tuple(newargs);
    Py_DECREF(newargs);
    return res;
}

static PyMappingMethods intersection_as_mapping = {
    .mp_subscript = intersection_getitem,
};

static PyObject *
intersection_parameters(PyObject *self, void *Py_UNUSED(unused))
{
    intersectionobject *alias = (intersectionobject *)self;
    if (intersection_init_parameters(alias) < 0) {
        return NULL;
    }
    return Py_NewRef(alias->parameters);
}

static PyObject *
intersection_name(PyObject *Py_UNUSED(self), void *Py_UNUSED(ignored))
{
    return PyUnicode_FromString("Intersection");
}

static PyObject *
intersection_origin(PyObject *Py_UNUSED(self), void *Py_UNUSED(ignored))
{
    return Py_NewRef(&_PyIntersection_Type);
}

static PyGetSetDef intersection_properties[] = {
    {"__name__", intersection_name, NULL,
     PyDoc_STR("Name of the type"), NULL},
    {"__qualname__", intersection_name, NULL,
     PyDoc_STR("Qualified name of the type"), NULL},
    {"__origin__", intersection_origin, NULL,
     PyDoc_STR("Always returns the type"), NULL},
    {"__parameters__", intersection_parameters, NULL,
     PyDoc_STR("Type variables in the types.IntersectionType."), NULL},
    {0}
};

static PyNumberMethods intersection_as_number = {
        .nb_and = _Py_intersection_type_and, // Add __and__ function
        .nb_or = _Py_union_type_or, // Add __or__ function
};

static const char* const cls_attrs[] = {
        "__module__",  // Required for compatibility with typing module
        NULL,
};

static PyObject *
intersection_getattro(PyObject *self, PyObject *name)
{
    intersectionobject *alias = (intersectionobject *)self;
    if (PyUnicode_Check(name)) {
        for (const char * const *p = cls_attrs; ; p++) {
            if (*p == NULL) {
                break;
            }
            if (_PyUnicode_EqualToASCIIString(name, *p)) {
                return PyObject_GetAttr((PyObject *) Py_TYPE(alias), name);
            }
        }
    }
    return PyObject_GenericGetAttr(self, name);
}

PyObject *
_Py_intersection_args(PyObject *self)
{
    assert(_PyIntersection_Check(self));
    return ((intersectionobject *) self)->args;
}

static PyObject *
call_typing_func_object(const char *name, PyObject **args, size_t nargs)
{
    PyObject *typing = PyImport_ImportModule("typing");
    if (typing == NULL) {
        return NULL;
    }
    PyObject *func = PyObject_GetAttrString(typing, name);
    if (func == NULL) {
        Py_DECREF(typing);
        return NULL;
    }
    PyObject *result = PyObject_Vectorcall(func, args, nargs, NULL);
    Py_DECREF(func);
    Py_DECREF(typing);
    return result;
}

static PyObject *
type_check(PyObject *arg, const char *msg)
{
    if (Py_IsNone(arg)) {
        // NoneType is immortal, so don't need an INCREF
        return (PyObject *)Py_TYPE(arg);
    }
    // Fast path to avoid calling into typing.py
    if (is_intersectionable(arg)) {
        return Py_NewRef(arg);
    }
    PyObject *message_str = PyUnicode_FromString(msg);
    if (message_str == NULL) {
        return NULL;
    }
    PyObject *args[2] = {arg, message_str};
    PyObject *result = call_typing_func_object("_type_check", args, 2);
    Py_DECREF(message_str);
    return result;
}

PyObject *
_Py_intersection_from_tuple(PyObject *args)
{
    intersectionbuilder ub;
    if (!intersectionbuilder_init(&ub, true)) {
        return NULL;
    }
    if (PyTuple_CheckExact(args)) {
        if (!intersectionbuilder_add_tuple(&ub, args)) {
            return NULL;
        }
    }
    else {
        if (!intersectionbuilder_add_single(&ub, args)) {
            return NULL;
        }
    }
    return make_intersection(&ub);
}

static PyObject *
intersection_class_getitem(PyObject *cls, PyObject *args)
{
    return _Py_intersection_from_tuple(args);
}

static PyObject *
intersection_mro_entries(PyObject *self, PyObject *args)
{
    return PyErr_Format(PyExc_TypeError,
                        "Cannot subclass %R", self);
}

static PyMethodDef intersection_methods[] = {
    {"__mro_entries__", intersection_mro_entries, METH_O},
    {"__class_getitem__", intersection_class_getitem, METH_O|METH_CLASS, PyDoc_STR("See PEP 585")},
    {0}
};

PyTypeObject _PyIntersection_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "typing.Intersection",
    .tp_doc = PyDoc_STR("Represent a intersection type\n"
              "\n"
              "E.g. for int & str"),
    .tp_basicsize = sizeof(intersectionobject),
    .tp_dealloc = intersectionobject_dealloc,
    .tp_alloc = PyType_GenericAlloc,
    .tp_free = PyObject_GC_Del,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = intersection_traverse,
    .tp_hash = intersection_hash,
    .tp_getattro = intersection_getattro,
    .tp_members = intersection_members,
    .tp_methods = intersection_methods,
    .tp_richcompare = intersection_richcompare,
    .tp_as_mapping = &intersection_as_mapping,
    .tp_as_number = &intersection_as_number,
    .tp_repr = intersection_repr,
    .tp_getset = intersection_properties,
    .tp_weaklistoffset = offsetof(intersectionobject, weakreflist),
};

static PyObject *
make_intersection(intersectionbuilder *ub)
{
    Py_ssize_t n = PyList_GET_SIZE(ub->args);
    if (n == 0) {
        PyErr_SetString(PyExc_TypeError, "Cannot take a Intersection of no types.");
        intersectionbuilder_finalize(ub);
        return NULL;
    }
    if (n == 1) {
        PyObject *result = PyList_GET_ITEM(ub->args, 0);
        Py_INCREF(result);
        intersectionbuilder_finalize(ub);
        return result;
    }

    PyObject *args = NULL, *hashable_args = NULL, *unhashable_args = NULL;
    args = PyList_AsTuple(ub->args);
    if (args == NULL) {
        goto error;
    }
    hashable_args = PyFrozenSet_New(ub->hashable_args);
    if (hashable_args == NULL) {
        goto error;
    }
    if (ub->unhashable_args != NULL) {
        unhashable_args = PyList_AsTuple(ub->unhashable_args);
        if (unhashable_args == NULL) {
            goto error;
        }
    }

    intersectionobject *result = PyObject_GC_New(intersectionobject, &_PyIntersection_Type);
    if (result == NULL) {
        goto error;
    }
    intersectionbuilder_finalize(ub);

    result->parameters = NULL;
    result->args = args;
    result->hashable_args = hashable_args;
    result->unhashable_args = unhashable_args;
    result->weakreflist = NULL;
    _PyObject_GC_TRACK(result);
    return (PyObject*)result;
error:
    Py_XDECREF(args);
    Py_XDECREF(hashable_args);
    Py_XDECREF(unhashable_args);
    intersectionbuilder_finalize(ub);
    return NULL;
}
