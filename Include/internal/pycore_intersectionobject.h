#ifndef Py_INTERNAL_INTERSECTIONOBJECT_H
#define Py_INTERNAL_INTERSECTIONOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// For extensions created by test_peg_generator
PyAPI_DATA(PyTypeObject) _PyIntersection_Type;
PyAPI_FUNC(PyObject *) _Py_intersection_type_and(PyObject *, PyObject *);

#define _PyIntersection_Check(op) Py_IS_TYPE((op), &_PyIntersection_Type)

#define _PyGenericAlias_Check(op) PyObject_TypeCheck((op), &Py_GenericAliasType)
extern PyObject *_Py_subs_parameters(PyObject *, PyObject *, PyObject *, PyObject *);
extern PyObject *_Py_make_parameters(PyObject *);
extern PyObject *_Py_intersection_args(PyObject *self);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_INTERSECTIONOBJECT_H */
