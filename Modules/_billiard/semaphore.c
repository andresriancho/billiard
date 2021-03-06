/*
 * A type which wraps a semaphore
 *
 * semaphore.c
 *
 * Copyright (c) 2006-2008, R Oudkerk
 * Licensed to PSF under a Contributor Agreement.
 */

#include "multiprocessing.h"

enum { RECURSIVE_MUTEX, SEMAPHORE };

typedef struct {
    PyObject_HEAD
    SEM_HANDLE handle;
    long last_tid;
    int count;
    int maxvalue;
    int kind;
    char *name;
} BilliardSemLockObject;

#define ISMINE(o) (o->count > 0 && PyThread_get_thread_ident() == o->last_tid)


#ifdef MS_WINDOWS

/*
 * Windows definitions
 */

#define SEM_FAILED NULL

#define SEM_CLEAR_ERROR() SetLastError(0)
#define SEM_GET_LAST_ERROR() GetLastError()
#define SEM_CREATE(name, val, max) CreateSemaphore(NULL, val, max, NULL)
#define SEM_CLOSE(sem) (CloseHandle(sem) ? 0 : -1)
#define SEM_GETVALUE(sem, pval) _Billiard_GetSemaphoreValue(sem, pval)
#define SEM_UNLINK(name) 0

static int
_Billiard_GetSemaphoreValue(HANDLE handle, long *value)
{
    long previous;

    switch (WaitForSingleObject(handle, 0)) {
    case WAIT_OBJECT_0:
        if (!ReleaseSemaphore(handle, 1, &previous))
            return MP_STANDARD_ERROR;
        *value = previous + 1;
        return 0;
    case WAIT_TIMEOUT:
        *value = 0;
        return 0;
    default:
        return MP_STANDARD_ERROR;
    }
}

static PyObject *
Billiard_semlock_acquire(BilliardSemLockObject *self, PyObject *args, PyObject *kwds)
{
    int blocking = 1;
    double timeout;
    PyObject *timeout_obj = Py_None;
    DWORD res, full_msecs, msecs, start, ticks;

    static char *kwlist[] = {"block", "timeout", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iO", kwlist,
                                     &blocking, &timeout_obj))
        return NULL;

    /* calculate timeout */
    if (!blocking) {
        full_msecs = 0;
    } else if (timeout_obj == Py_None) {
        full_msecs = INFINITE;
    } else {
        timeout = PyFloat_AsDouble(timeout_obj);
        if (PyErr_Occurred())
            return NULL;
        timeout *= 1000.0;      /* convert to millisecs */
        if (timeout < 0.0) {
            timeout = 0.0;
        } else if (timeout >= 0.5 * INFINITE) { /* 25 days */
            PyErr_SetString(PyExc_OverflowError,
                            "timeout is too large");
            return NULL;
        }
        full_msecs = (DWORD)(timeout + 0.5);
    }

    /* check whether we already own the lock */
    if (self->kind == RECURSIVE_MUTEX && ISMINE(self)) {
        ++self->count;
        Py_RETURN_TRUE;
    }

    /* check whether we can acquire without blocking */
    if (WaitForSingleObject(self->handle, 0) == WAIT_OBJECT_0) {
        self->last_tid = GetCurrentThreadId();
        ++self->count;
        Py_RETURN_TRUE;
    }

    msecs = full_msecs;
    start = GetTickCount();

    for ( ; ; ) {
        HANDLE handles[2] = {self->handle, sigint_event};

        /* do the wait */
        Py_BEGIN_ALLOW_THREADS
        ResetEvent(sigint_event);
        res = WaitForMultipleObjects(2, handles, FALSE, msecs);
        Py_END_ALLOW_THREADS

        /* handle result */
        if (res != WAIT_OBJECT_0 + 1)
            break;

        /* got SIGINT so give signal handler a chance to run */
        Sleep(1);

        /* if this is main thread let KeyboardInterrupt be raised */
        if (PyErr_CheckSignals())
            return NULL;

        /* recalculate timeout */
        if (msecs != INFINITE) {
            ticks = GetTickCount();
            if ((DWORD)(ticks - start) >= full_msecs)
                Py_RETURN_FALSE;
            msecs = full_msecs - (ticks - start);
        }
    }

    /* handle result */
    switch (res) {
    case WAIT_TIMEOUT:
        Py_RETURN_FALSE;
    case WAIT_OBJECT_0:
        self->last_tid = GetCurrentThreadId();
        ++self->count;
        Py_RETURN_TRUE;
    case WAIT_FAILED:
        return PyErr_SetFromWindowsErr(0);
    default:
        PyErr_Format(PyExc_RuntimeError, "WaitForSingleObject() or "
                     "WaitForMultipleObjects() gave unrecognized "
                     "value %d", res);
        return NULL;
    }
}

static PyObject *
Billiard_semlock_release(BilliardSemLockObject *self, PyObject *args)
{
    if (self->kind == RECURSIVE_MUTEX) {
        if (!ISMINE(self)) {
            PyErr_SetString(PyExc_AssertionError, "attempt to "
                            "release recursive lock not owned "
                            "by thread");
            return NULL;
        }
        if (self->count > 1) {
            --self->count;
            Py_RETURN_NONE;
        }
        assert(self->count == 1);
    }

    if (!ReleaseSemaphore(self->handle, 1, NULL)) {
        if (GetLastError() == ERROR_TOO_MANY_POSTS) {
            PyErr_SetString(PyExc_ValueError, "semaphore or lock "
                            "released too many times");
            return NULL;
        } else {
            return PyErr_SetFromWindowsErr(0);
        }
    }

    --self->count;
    Py_RETURN_NONE;
}

#else /* !MS_WINDOWS */

/*
 * Unix definitions
 */

#define SEM_CLEAR_ERROR()
#define SEM_GET_LAST_ERROR() 0
#define SEM_CREATE(name, val, max) sem_open(name, O_CREAT | O_EXCL, 0600, val)
#define SEM_CLOSE(sem) sem_close(sem)
#define SEM_GETVALUE(sem, pval) sem_getvalue(sem, pval)
#define SEM_UNLINK(name) sem_unlink(name)

#ifndef HAVE_SEM_UNLINK
#  define sem_unlink(name) 0
#endif

//#ifndef HAVE_SEM_TIMEDWAIT
#  define sem_timedwait(sem,deadline) Billiard_sem_timedwait_save(sem,deadline,_save)

int
Billiard_sem_timedwait_save(sem_t *sem, struct timespec *deadline, PyThreadState *_save)
{
    int res;
    unsigned long delay, difference;
    struct timeval now, tvdeadline, tvdelay;

    errno = 0;
    tvdeadline.tv_sec = deadline->tv_sec;
    tvdeadline.tv_usec = deadline->tv_nsec / 1000;

    for (delay = 0 ; ; delay += 1000) {
        /* poll */
        if (sem_trywait(sem) == 0)
            return 0;
        else if (errno != EAGAIN)
            return MP_STANDARD_ERROR;

        /* get current time */
        if (gettimeofday(&now, NULL) < 0)
            return MP_STANDARD_ERROR;

        /* check for timeout */
        if (tvdeadline.tv_sec < now.tv_sec ||
            (tvdeadline.tv_sec == now.tv_sec &&
             tvdeadline.tv_usec <= now.tv_usec)) {
            errno = ETIMEDOUT;
            return MP_STANDARD_ERROR;
        }

        /* calculate how much time is left */
        difference = (tvdeadline.tv_sec - now.tv_sec) * 1000000 +
            (tvdeadline.tv_usec - now.tv_usec);

        /* check delay not too long -- maximum is 20 msecs */
        if (delay > 20000)
            delay = 20000;
        if (delay > difference)
            delay = difference;

        /* sleep */
        tvdelay.tv_sec = delay / 1000000;
        tvdelay.tv_usec = delay % 1000000;
        if (select(0, NULL, NULL, NULL, &tvdelay) < 0)
            return MP_STANDARD_ERROR;

        /* check for signals */
        Py_BLOCK_THREADS
        res = PyErr_CheckSignals();
        Py_UNBLOCK_THREADS

        if (res) {
            errno = EINTR;
            return MP_EXCEPTION_HAS_BEEN_SET;
        }
    }
}

//#endif /* !HAVE_SEM_TIMEDWAIT */

static PyObject *
Billiard_semlock_acquire(BilliardSemLockObject *self, PyObject *args, PyObject *kwds)
{
    int blocking = 1, res;
    double timeout;
    PyObject *timeout_obj = Py_None;
    struct timespec deadline = {0};
    struct timeval now;
    long sec, nsec;

    static char *kwlist[] = {"block", "timeout", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|iO", kwlist,
                                     &blocking, &timeout_obj))
        return NULL;

    if (self->kind == RECURSIVE_MUTEX && ISMINE(self)) {
        ++self->count;
        Py_RETURN_TRUE;
    }

    if (timeout_obj != Py_None) {
        timeout = PyFloat_AsDouble(timeout_obj);
        if (PyErr_Occurred())
            return NULL;
        if (timeout < 0.0)
            timeout = 0.0;

        if (gettimeofday(&now, NULL) < 0) {
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }
        sec = (long) timeout;
        nsec = (long) (1e9 * (timeout - sec) + 0.5);
        deadline.tv_sec = now.tv_sec + sec;
        deadline.tv_nsec = now.tv_usec * 1000 + nsec;
        deadline.tv_sec += (deadline.tv_nsec / 1000000000);
        deadline.tv_nsec %= 1000000000;
    }

    do {
        Py_BEGIN_ALLOW_THREADS
        if (blocking && timeout_obj == Py_None)
            res = sem_wait(self->handle);
        else if (!blocking)
            res = sem_trywait(self->handle);
        else
            res = sem_timedwait(self->handle, &deadline);
        Py_END_ALLOW_THREADS
        if (res == MP_EXCEPTION_HAS_BEEN_SET)
            break;
    } while (res < 0 && errno == EINTR && !PyErr_CheckSignals());

    if (res < 0) {
        if (errno == EAGAIN || errno == ETIMEDOUT)
            Py_RETURN_FALSE;
        else if (errno == EINTR)
            return NULL;
        else
            return PyErr_SetFromErrno(PyExc_OSError);
    }

    ++self->count;
    self->last_tid = PyThread_get_thread_ident();

    Py_RETURN_TRUE;
}

static PyObject *
Billiard_semlock_release(BilliardSemLockObject *self, PyObject *args)
{
    if (self->kind == RECURSIVE_MUTEX) {
        if (!ISMINE(self)) {
            PyErr_SetString(PyExc_AssertionError, "attempt to "
                            "release recursive lock not owned "
                            "by thread");
            return NULL;
        }
        if (self->count > 1) {
            --self->count;
            Py_RETURN_NONE;
        }
        assert(self->count == 1);
    } else {
#ifdef HAVE_BROKEN_SEM_GETVALUE
        /* We will only check properly the maxvalue == 1 case */
        if (self->maxvalue == 1) {
            /* make sure that already locked */
            if (sem_trywait(self->handle) < 0) {
                if (errno != EAGAIN) {
                    PyErr_SetFromErrno(PyExc_OSError);
                    return NULL;
                }
                /* it is already locked as expected */
            } else {
                /* it was not locked so undo wait and raise  */
                if (sem_post(self->handle) < 0) {
                    PyErr_SetFromErrno(PyExc_OSError);
                    return NULL;
                }
                PyErr_SetString(PyExc_ValueError, "semaphore "
                                "or lock released too many "
                                "times");
                return NULL;
            }
        }
#else
        int sval;

        /* This check is not an absolute guarantee that the semaphore
           does not rise above maxvalue. */
        if (sem_getvalue(self->handle, &sval) < 0) {
            return PyErr_SetFromErrno(PyExc_OSError);
        } else if (sval >= self->maxvalue) {
            PyErr_SetString(PyExc_ValueError, "semaphore or lock "
                            "released too many times");
            return NULL;
        }
#endif
    }

    if (sem_post(self->handle) < 0)
        return PyErr_SetFromErrno(PyExc_OSError);

    --self->count;
    Py_RETURN_NONE;
}

#endif /* !MS_WINDOWS */

/*
 * All platforms
 */

static PyObject *
Billiard_newsemlockobject(PyTypeObject *type, SEM_HANDLE handle, int kind, int maxvalue,
                 char *name)
{
    BilliardSemLockObject *self;

    self = PyObject_New(BilliardSemLockObject, type);
    if (!self)
        return NULL;
    self->handle = handle;
    self->kind = kind;
    self->count = 0;
    self->last_tid = 0;
    self->maxvalue = maxvalue;
    self->name = name;
    return (PyObject*)self;
}

static PyObject *
Billiard_semlock_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    SEM_HANDLE handle = SEM_FAILED;
    int kind, maxvalue, value, unlink;
    PyObject *result;
    char *name, *name_copy = NULL;
    static char *kwlist[] = {"kind", "value", "maxvalue", "name", "unlink",
                             NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "iiisi", kwlist,
                                     &kind, &value, &maxvalue, &name, &unlink))
        return NULL;

    if (kind != RECURSIVE_MUTEX && kind != SEMAPHORE) {
        PyErr_SetString(PyExc_ValueError, "unrecognized kind");
        return NULL;
    }

    if (!unlink) {
        name_copy = PyMem_Malloc(strlen(name) + 1);
        if (name_copy == NULL)
            goto failure;
        strcpy(name_copy, name);
    }

    SEM_CLEAR_ERROR();
    handle = SEM_CREATE(name, value, maxvalue);
    /* On Windows we should fail if GetLastError()==ERROR_ALREADY_EXISTS */
    if (handle == SEM_FAILED || SEM_GET_LAST_ERROR() != 0)
        goto failure;

    if (unlink && SEM_UNLINK(name) < 0)
        goto failure;

    result = Billiard_newsemlockobject(type, handle, kind, maxvalue, name_copy);
    if (!result)
        goto failure;

    return result;

  failure:
    if (handle != SEM_FAILED)
        SEM_CLOSE(handle);
    PyMem_Free(name_copy);
    Billiard_SetError(NULL, MP_STANDARD_ERROR);
    return NULL;
}

static PyObject *
Billiard_semlock_rebuild(PyTypeObject *type, PyObject *args)
{
    SEM_HANDLE handle;
    int kind, maxvalue;
    char *name;

    if (!PyArg_ParseTuple(args, F_SEM_HANDLE "iiz",
                          &handle, &kind, &maxvalue, &name))
        return NULL;

#ifndef MS_WINDOWS
    if (name != NULL) {
        handle = sem_open(name, 0);
        if (handle == SEM_FAILED)
            return NULL;
    }
#endif

    return Billiard_newsemlockobject(type, handle, kind, maxvalue, name);
}

static void
Billiard_semlock_dealloc(BilliardSemLockObject* self)
{
    if (self->handle != SEM_FAILED)
        SEM_CLOSE(self->handle);
    PyMem_Free(self->name);
    PyObject_Del(self);
}

static PyObject *
Billiard_semlock_count(BilliardSemLockObject *self)
{
    return PyInt_FromLong((long)self->count);
}

static PyObject *
Billiard_semlock_ismine(BilliardSemLockObject *self)
{
    /* only makes sense for a lock */
    return PyBool_FromLong(ISMINE(self));
}

static PyObject *
Billiard_semlock_getvalue(BilliardSemLockObject *self)
{
#ifdef HAVE_BROKEN_SEM_GETVALUE
    PyErr_SetNone(PyExc_NotImplementedError);
    return NULL;
#else
    int sval;
    if (SEM_GETVALUE(self->handle, &sval) < 0)
        return Billiard_SetError(NULL, MP_STANDARD_ERROR);
    /* some posix implementations use negative numbers to indicate
       the number of waiting threads */
    if (sval < 0)
        sval = 0;
    return PyInt_FromLong((long)sval);
#endif
}

static PyObject *
Billiard_semlock_iszero(BilliardSemLockObject *self)
{
#ifdef HAVE_BROKEN_SEM_GETVALUE
    if (sem_trywait(self->handle) < 0) {
        if (errno == EAGAIN)
            Py_RETURN_TRUE;
        return Billiard_SetError(NULL, MP_STANDARD_ERROR);
    } else {
        if (sem_post(self->handle) < 0)
            return Billiard_SetError(NULL, MP_STANDARD_ERROR);
        Py_RETURN_FALSE;
    }
#else
    int sval;
    if (SEM_GETVALUE(self->handle, &sval) < 0)
        return Billiard_SetError(NULL, MP_STANDARD_ERROR);
    return PyBool_FromLong((long)sval == 0);
#endif
}

static PyObject *
Billiard_semlock_afterfork(BilliardSemLockObject *self)
{
    self->count = 0;
    Py_RETURN_NONE;
}

PyObject *
Billiard_semlock_unlink(PyObject *ignore, PyObject *args)
{
    char *name;

    if (!PyArg_ParseTuple(args, "s", &name))
        return NULL;

    if (SEM_UNLINK(name) < 0) {
        Billiard_SetError(NULL, MP_STANDARD_ERROR);
        return NULL;
    }

    Py_RETURN_NONE;
}

/*
 * Semaphore methods
 */

static PyMethodDef Billiard_semlock_methods[] = {
    {"acquire", (PyCFunction)Billiard_semlock_acquire, METH_VARARGS | METH_KEYWORDS,
     "acquire the semaphore/lock"},
    {"release", (PyCFunction)Billiard_semlock_release, METH_NOARGS,
     "release the semaphore/lock"},
    {"__enter__", (PyCFunction)Billiard_semlock_acquire, METH_VARARGS | METH_KEYWORDS,
     "enter the semaphore/lock"},
    {"__exit__", (PyCFunction)Billiard_semlock_release, METH_VARARGS,
     "exit the semaphore/lock"},
    {"_count", (PyCFunction)Billiard_semlock_count, METH_NOARGS,
     "num of `acquire()`s minus num of `release()`s for this process"},
    {"_is_mine", (PyCFunction)Billiard_semlock_ismine, METH_NOARGS,
     "whether the lock is owned by this thread"},
    {"_get_value", (PyCFunction)Billiard_semlock_getvalue, METH_NOARGS,
     "get the value of the semaphore"},
    {"_is_zero", (PyCFunction)Billiard_semlock_iszero, METH_NOARGS,
     "returns whether semaphore has value zero"},
    {"_rebuild", (PyCFunction)Billiard_semlock_rebuild, METH_VARARGS | METH_CLASS,
     ""},
    {"_after_fork", (PyCFunction)Billiard_semlock_afterfork, METH_NOARGS,
     "rezero the net acquisition count after fork()"},
    {"sem_unlink", (PyCFunction)Billiard_semlock_unlink, METH_VARARGS | METH_STATIC,
     "unlink the named semaphore using sem_unlink()"},

    {NULL}
};

/*
 * Member table
 */

static PyMemberDef Billiard_semlock_members[] = {
    {"handle", T_SEM_HANDLE, offsetof(BilliardSemLockObject, handle), READONLY,
     ""},
    {"kind", T_INT, offsetof(BilliardSemLockObject, kind), READONLY,
     ""},
    {"maxvalue", T_INT, offsetof(BilliardSemLockObject, maxvalue), READONLY,
     ""},
    {"name", T_STRING, offsetof(BilliardSemLockObject, name), READONLY,
     ""},
    {NULL}
};

/*
 * Semaphore type
 */

PyTypeObject BilliardSemLockType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    /* tp_name           */ "_billiard.SemLock",
    /* tp_basicsize      */ sizeof(BilliardSemLockObject),
    /* tp_itemsize       */ 0,
    /* tp_dealloc        */ (destructor)Billiard_semlock_dealloc,
    /* tp_print          */ 0,
    /* tp_getattr        */ 0,
    /* tp_setattr        */ 0,
    /* tp_compare        */ 0,
    /* tp_repr           */ 0,
    /* tp_as_number      */ 0,
    /* tp_as_sequence    */ 0,
    /* tp_as_mapping     */ 0,
    /* tp_hash           */ 0,
    /* tp_call           */ 0,
    /* tp_str            */ 0,
    /* tp_getattro       */ 0,
    /* tp_setattro       */ 0,
    /* tp_as_buffer      */ 0,
    /* tp_flags          */ Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    /* tp_doc            */ "Semaphore/Mutex type",
    /* tp_traverse       */ 0,
    /* tp_clear          */ 0,
    /* tp_richcompare    */ 0,
    /* tp_weaklistoffset */ 0,
    /* tp_iter           */ 0,
    /* tp_iternext       */ 0,
    /* tp_methods        */ Billiard_semlock_methods,
    /* tp_members        */ Billiard_semlock_members,
    /* tp_getset         */ 0,
    /* tp_base           */ 0,
    /* tp_dict           */ 0,
    /* tp_descr_get      */ 0,
    /* tp_descr_set      */ 0,
    /* tp_dictoffset     */ 0,
    /* tp_init           */ 0,
    /* tp_alloc          */ 0,
    /* tp_new            */ Billiard_semlock_new,
};
