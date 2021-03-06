/*
  Copyright (c) 2013, Ilan Schnell, Continuum Analytics, Inc.
  Python bindings to picosat (http://fmv.jku.at/picosat/)
  This file is published under the same license as picosat itself, which
  uses an MIT style license.
*/

#include <Python.h>

#ifdef _MSC_VER
#define NGETRUSAGE
#define inline __inline
#endif

#include "picosat.h"
#ifndef DONT_INCLUDE_PICOSAT
#include "picosat.c"
#endif

#if PY_MAJOR_VERSION >= 3
#define IS_PY3K
#endif

#ifdef IS_PY3K
#define PyInt_FromLong  PyLong_FromLong
#define IS_INT(x)  (PyLong_Check(x))
#else
#define IS_INT(x)  (PyInt_Check(x) || PyLong_Check(x))
#endif


/* the following three adapter functions are used as arguments to
   picosat_minit, such that picosat used the Python memory manager */
inline static void *py_malloc(void *mmgr, size_t bytes)
{
    return PyMem_Malloc(bytes);
}

inline static void *py_realloc(void *mmgr, void *ptr, size_t old, size_t new)
{
    return PyMem_Realloc(ptr, new);
}

inline static void py_free(void *mmgr, void *ptr, size_t bytes)
{
    PyMem_Free(ptr);
}

/* Add the inverse of the (current) solution to the clauses.
   This function is essentially the same as the function blocksol in app.c
   in the picosat source. */
static int blocksol(PicoSAT *picosat, signed char *mem)
{
    int max_idx, i;

    max_idx = picosat_variables(picosat);
    if (mem == NULL) {
        mem = PyMem_Malloc(max_idx + 1);
        if (mem == NULL) {
            PyErr_NoMemory();
            return -1;
        }
    }
    for (i = 1; i <= max_idx; i++)
        mem[i] = (picosat_deref(picosat, i) > 0) ? 1 : -1;

    for (i = 1; i <= max_idx; i++)
        picosat_add(picosat, (mem[i] < 0) ? i : -i);

    picosat_add(picosat, 0);
    return 0;
}

static int add_clause(PicoSAT *picosat, PyObject *clause)
{
    PyObject *lit;              /* the literals are integers */
    Py_ssize_t n, i;
    int v;

    if (!PyList_Check(clause)) {
        PyErr_SetString(PyExc_TypeError, "list expected");
        return -1;
    }

    n = PyList_Size(clause);
    for (i = 0; i < n; i++) {
        lit = PyList_GetItem(clause, i);
        if (lit == NULL)
            return -1;
        if (!IS_INT(lit))  {
            PyErr_SetString(PyExc_TypeError, "interger expected");
            return -1;
        }
        v = PyLong_AsLong(lit);
        if (v == 0) {
            PyErr_SetString(PyExc_ValueError, "non-zero interger expected");
            return -1;
        }
        picosat_add(picosat, v);
    }
    picosat_add(picosat, 0);
    return 0;
}

static int add_clauses(PicoSAT *picosat, PyObject *clauses)
{
    PyObject *item;             /* each clause is a list of intergers */
    Py_ssize_t n, i;

    if (!PyList_Check(clauses)) {
        PyErr_SetString(PyExc_TypeError, "list expected");
        return -1;
    }

    n = PyList_Size(clauses);
    for (i = 0; i < n; i++) {
        item = PyList_GetItem(clauses, i);
        if (item == NULL)
            return -1;
        if (add_clause(picosat, item) < 0)
            return -1;
    }
    return 0;
}

static PicoSAT* setup_picosat(PyObject *args, PyObject *kwds)
{
    PicoSAT *picosat;
    PyObject *clauses;          /* list of clauses */
    int vars = -1, verbose = 0;
    unsigned long long prop_limit = 0;
    static char* kwlist[] = {"clauses",
                             "vars", "verbose", "prop_limit", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|iiK:(iter)solve", kwlist,
                                     &clauses,
                                     &vars, &verbose, &prop_limit))
        return NULL;

    picosat = picosat_minit(NULL, py_malloc, py_realloc, py_free);
    picosat_set_verbosity(picosat, verbose);
    if (vars != -1)
        picosat_adjust(picosat, vars);

    if (prop_limit)
        picosat_set_propagation_limit(picosat, prop_limit);

    if (add_clauses(picosat, clauses) < 0) {
        picosat_reset(picosat);
        return NULL;
    }

    if (verbose >= 2)
        picosat_print(picosat, stdout);

    return picosat;
}

static PyObject* get_solution(PicoSAT *picosat)
{
    PyObject *list;
    int max_idx, i, v;

    max_idx = picosat_variables(picosat);
    list = PyList_New((Py_ssize_t) max_idx);
    if (list == NULL) {
        picosat_reset(picosat);
        return NULL;
    }
    for (i = 1; i <= max_idx; i++) {
        v = picosat_deref(picosat, i);
        assert(v == -1 || v == 1);
        if (PyList_SetItem(list, (Py_ssize_t) (i - 1),
                           PyInt_FromLong((long) (v * i))) < 0) {
            Py_DECREF(list);
            picosat_reset(picosat);
            return NULL;
        }
    }
    return list;
}

static PyObject* solve(PyObject *self, PyObject *args, PyObject *kwds)
{
    PicoSAT *picosat;
    PyObject *result = NULL;    /* return value */
    int res;

    picosat = setup_picosat(args, kwds);
    if (picosat == NULL)
        return NULL;

    Py_BEGIN_ALLOW_THREADS      /* release GIL */
    res = picosat_sat(picosat, -1);
    Py_END_ALLOW_THREADS

    switch (res) {
    case PICOSAT_SATISFIABLE:
        result = get_solution(picosat);
        break;

    case PICOSAT_UNSATISFIABLE:
        result = PyUnicode_FromString("UNSAT");
        break;

    case PICOSAT_UNKNOWN:
        result = PyUnicode_FromString("UNKNOWN");
        break;

    default:
        PyErr_Format(PyExc_SystemError, "picosat return value: %d", res);
    }

    picosat_reset(picosat);
    return result;
}

/*********************** Solution Iterator *********************/

typedef struct {
    PyObject_HEAD
    PicoSAT *picosat;
    signed char *mem;           /* temporary storage */
} soliterobject;

static PyTypeObject SolIter_Type;

#define SolIter_Check(op)  PyObject_TypeCheck(op, &SolIter_Type)

static PyObject* itersolve(PyObject *self, PyObject *args, PyObject *kwds)
{
    soliterobject *it;          /* iterator to be returned */

    it = PyObject_GC_New(soliterobject, &SolIter_Type);
    if (it == NULL)
        return NULL;

    it->picosat = setup_picosat(args, kwds);
    if (it->picosat == NULL)
        return NULL;

    it->mem = NULL;
    PyObject_GC_Track(it);
    return (PyObject *) it;
}

static PyObject* soliter_next(soliterobject *it)
{
    PyObject *result = NULL;    /* return value */
    int res;

    assert(SolIter_Check(it));

    Py_BEGIN_ALLOW_THREADS      /* release GIL */
    res = picosat_sat(it->picosat, -1);
    Py_END_ALLOW_THREADS

    switch (res) {
    case PICOSAT_SATISFIABLE:
        result = get_solution(it->picosat);
        if (result == NULL) {
            PyErr_SetString(PyExc_SystemError, "failed to create list");
            return NULL;
        }
        /* add inverse solution to the clauses,
           so that next solution can be generated */
        if (blocksol(it->picosat, it->mem) < 0)
            return NULL;
        break;

    case PICOSAT_UNSATISFIABLE:
    case PICOSAT_UNKNOWN:
        /* no more solutions -- stop iteration */
        break;

    default:
        PyErr_Format(PyExc_SystemError, "picosat return value: %d", res);
    }
    return result;
}

static void soliter_dealloc(soliterobject *it)
{
    PyObject_GC_UnTrack(it);
    if (it->mem)
        PyMem_Free(it->mem);
    picosat_reset(it->picosat);
    PyObject_GC_Del(it);
}

static int soliter_traverse(soliterobject *it, visitproc visit, void *arg)
{
    return 0;
}

static PyTypeObject SolIter_Type = {
#ifdef IS_PY3K
    PyVarObject_HEAD_INIT(&SolIter_Type, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                        /* ob_size */
#endif
    "soliterator",                            /* tp_name */
    sizeof(soliterobject),                    /* tp_basicsize */
    0,                                        /* tp_itemsize */
    /* methods */
    (destructor) soliter_dealloc,             /* tp_dealloc */
    0,                                        /* tp_print */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_compare */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    PyObject_GenericGetAttr,                  /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,  /* tp_flags */
    0,                                        /* tp_doc */
    (traverseproc) soliter_traverse,          /* tp_traverse */
    0,                                        /* tp_clear */
    0,                                        /* tp_richcompare */
    0,                                        /* tp_weaklistoffset */
    PyObject_SelfIter,                        /* tp_iter */
    (iternextfunc) soliter_next,              /* tp_iternext */
    0,                                        /* tp_methods */
};

/*************************** Method definitions *************************/

/* declaration of methods supported by this module */
static PyMethodDef module_functions[] = {
    {"solve",     (PyCFunction) solve,     METH_VARARGS | METH_KEYWORDS},
    {"itersolve", (PyCFunction) itersolve, METH_VARARGS | METH_KEYWORDS},
    {NULL,        NULL}  /* sentinel */
};

/* initialization routine for the shared libary */
#ifdef IS_PY3K
static PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT, "pycosat", 0, -1, module_functions,
};
PyMODINIT_FUNC PyInit_pycosat(void)
#else
PyMODINIT_FUNC initpycosat(void)
#endif
{
    PyObject *m;

#ifdef IS_PY3K
    m = PyModule_Create(&moduledef);
    if (m == NULL)
        return NULL;
#else
    m = Py_InitModule3("pycosat", module_functions, 0);
    if (m == NULL)
        return;
#endif

#ifdef PYCOSAT_VERSION
    PyModule_AddObject(m, "__version__",
                       PyUnicode_FromString(PYCOSAT_VERSION));
#endif

#ifdef IS_PY3K
    return m;
#endif
}
