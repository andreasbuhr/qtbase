/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qeventdispatcher_glib_p.h"
#include "qeventdispatcher_unix_p.h"

#include <private/qthread_p.h>

#include "qcoreapplication.h"
#include "qsocketnotifier.h"

#include <QtCore/qlist.h>
#include <QtCore/qpair.h>

#include <glib.h>

// declaring our variant of g_main_context_iteration
gboolean
qt_g_main_context_iteration (GMainContext *context, gboolean may_block, gboolean all_priorities);

QT_BEGIN_NAMESPACE

struct GPollFDWithQSocketNotifier
{
    GPollFD pollfd;
    QSocketNotifier *socketNotifier;
};

struct GSocketNotifierSource
{
    GSource source;
    QList<GPollFDWithQSocketNotifier *> pollfds;
};

static gboolean socketNotifierSourcePrepare(GSource *, gint *timeout)
{
    if (timeout)
        *timeout = -1;
    return false;
}

static gboolean socketNotifierSourceCheck(GSource *source)
{
    GSocketNotifierSource *src = reinterpret_cast<GSocketNotifierSource *>(source);

    bool pending = false;
    for (int i = 0; !pending && i < src->pollfds.count(); ++i) {
        GPollFDWithQSocketNotifier *p = src->pollfds.at(i);

        if (p->pollfd.revents & G_IO_NVAL) {
            // disable the invalid socket notifier
            static const char *t[] = { "Read", "Write", "Exception" };
            qWarning("QSocketNotifier: Invalid socket %d and type '%s', disabling...",
                     p->pollfd.fd, t[int(p->socketNotifier->type())]);
            // ### note, modifies src->pollfds!
            p->socketNotifier->setEnabled(false);
            i--;
        } else {
            pending = pending || ((p->pollfd.revents & p->pollfd.events) != 0);
        }
    }

    return pending;
}

static gboolean socketNotifierSourceDispatch(GSource *source, GSourceFunc, gpointer)
{
    QEvent event(QEvent::SockAct);

    GSocketNotifierSource *src = reinterpret_cast<GSocketNotifierSource *>(source);
    for (int i = 0; i < src->pollfds.count(); ++i) {
        GPollFDWithQSocketNotifier *p = src->pollfds.at(i);

        if ((p->pollfd.revents & p->pollfd.events) != 0)
            QCoreApplication::sendEvent(p->socketNotifier, &event);
    }

    return true; // ??? don't remove, right?
}

static GSourceFuncs socketNotifierSourceFuncs = {
    socketNotifierSourcePrepare,
    socketNotifierSourceCheck,
    socketNotifierSourceDispatch,
    nullptr,
    nullptr,
    nullptr
};

struct GTimerSource
{
    GSource source;
    QTimerInfoList timerList;
    QEventLoop::ProcessEventsFlags processEventsFlags;
    bool runWithIdlePriority;
};

static gboolean timerSourcePrepareHelper(GTimerSource *src, gint *timeout)
{
    timespec tv = { 0l, 0l };
    if (!(src->processEventsFlags & QEventLoop::X11ExcludeTimers) && src->timerList.timerWait(tv))
        *timeout = (tv.tv_sec * 1000) + ((tv.tv_nsec + 999999) / 1000 / 1000);
    else
        *timeout = -1;

    return (*timeout == 0);
}

static gboolean timerSourceCheckHelper(GTimerSource *src)
{
    if (src->timerList.isEmpty()
        || (src->processEventsFlags & QEventLoop::X11ExcludeTimers))
        return false;

    if (src->timerList.updateCurrentTime() < src->timerList.constFirst()->timeout)
        return false;

    return true;
}

static gboolean timerSourcePrepare(GSource *source, gint *timeout)
{
    gint dummy;
    if (!timeout)
        timeout = &dummy;

    GTimerSource *src = reinterpret_cast<GTimerSource *>(source);
    if (src->runWithIdlePriority) {
        if (timeout)
            *timeout = -1;
        return false;
    }

    return timerSourcePrepareHelper(src, timeout);
}

static gboolean timerSourceCheck(GSource *source)
{
    GTimerSource *src = reinterpret_cast<GTimerSource *>(source);
    if (src->runWithIdlePriority)
        return false;
    return timerSourceCheckHelper(src);
}

static gboolean timerSourceDispatch(GSource *source, GSourceFunc, gpointer)
{
    GTimerSource *timerSource = reinterpret_cast<GTimerSource *>(source);
    if (timerSource->processEventsFlags & QEventLoop::X11ExcludeTimers)
        return true;
    timerSource->runWithIdlePriority = true;
    (void) timerSource->timerList.activateTimers();
    return true; // ??? don't remove, right again?
}

static GSourceFuncs timerSourceFuncs = {
    timerSourcePrepare,
    timerSourceCheck,
    timerSourceDispatch,
    nullptr,
    nullptr,
    nullptr
};

struct GIdleTimerSource
{
    GSource source;
    GTimerSource *timerSource;
};

static gboolean idleTimerSourcePrepare(GSource *source, gint *timeout)
{
    GIdleTimerSource *idleTimerSource = reinterpret_cast<GIdleTimerSource *>(source);
    GTimerSource *timerSource = idleTimerSource->timerSource;
    if (!timerSource->runWithIdlePriority) {
        // Yield to the normal priority timer source
        if (timeout)
            *timeout = -1;
        return false;
    }

    return timerSourcePrepareHelper(timerSource, timeout);
}

static gboolean idleTimerSourceCheck(GSource *source)
{
    GIdleTimerSource *idleTimerSource = reinterpret_cast<GIdleTimerSource *>(source);
    GTimerSource *timerSource = idleTimerSource->timerSource;
    if (!timerSource->runWithIdlePriority) {
        // Yield to the normal priority timer source
        return false;
    }
    return timerSourceCheckHelper(timerSource);
}

static gboolean idleTimerSourceDispatch(GSource *source, GSourceFunc, gpointer)
{
    GTimerSource *timerSource = reinterpret_cast<GIdleTimerSource *>(source)->timerSource;
    (void) timerSourceDispatch(&timerSource->source, nullptr, nullptr);
    return true;
}

static GSourceFuncs idleTimerSourceFuncs = {
    idleTimerSourcePrepare,
    idleTimerSourceCheck,
    idleTimerSourceDispatch,
    nullptr,
    nullptr,
    nullptr
};

struct GPostEventSource
{
    GSource source;
    QAtomicInt serialNumber;
    int lastSerialNumber;
    QEventDispatcherGlibPrivate *d;
};

static gboolean postEventSourcePrepare(GSource *s, gint *timeout)
{
    QThreadData *data = QThreadData::current();
    if (!data)
        return false;

    gint dummy;
    if (!timeout)
        timeout = &dummy;
    const bool canWait = data->canWaitLocked();
    *timeout = canWait ? -1 : 0;

    GPostEventSource *source = reinterpret_cast<GPostEventSource *>(s);
    source->d->wakeUpCalled = source->serialNumber.loadRelaxed() != source->lastSerialNumber;
    return !canWait || source->d->wakeUpCalled;
}

static gboolean postEventSourceCheck(GSource *source)
{
    return postEventSourcePrepare(source, nullptr);
}

static gboolean postEventSourceDispatch(GSource *s, GSourceFunc, gpointer)
{
    GPostEventSource *source = reinterpret_cast<GPostEventSource *>(s);
    source->lastSerialNumber = source->serialNumber.loadRelaxed();
    QCoreApplication::sendPostedEvents();
    source->d->runTimersOnceWithNormalPriority();
    return true; // i dunno, george...
}

static GSourceFuncs postEventSourceFuncs = {
    postEventSourcePrepare,
    postEventSourceCheck,
    postEventSourceDispatch,
    nullptr,
    nullptr,
    nullptr
};


QEventDispatcherGlibPrivate::QEventDispatcherGlibPrivate(GMainContext *context)
    : mainContext(context)
{
#if GLIB_MAJOR_VERSION == 2 && GLIB_MINOR_VERSION < 32
    if (qEnvironmentVariableIsEmpty("QT_NO_THREADED_GLIB")) {
        static QBasicMutex mutex;
        QMutexLocker locker(&mutex);
        if (!g_thread_supported())
            g_thread_init(NULL);
    }
#endif

    if (mainContext) {
        g_main_context_ref(mainContext);
    } else {
        QCoreApplication *app = QCoreApplication::instance();
        if (app && QThread::currentThread() == app->thread()) {
            mainContext = g_main_context_default();
            g_main_context_ref(mainContext);
        } else {
            mainContext = g_main_context_new();
        }
    }

#if GLIB_CHECK_VERSION (2, 22, 0)
    g_main_context_push_thread_default (mainContext);
#endif

    // setup post event source
    GSource *source = g_source_new(&postEventSourceFuncs, sizeof(GPostEventSource));
    g_source_set_name(source, "[Qt] GPostEventSource");
    postEventSource = reinterpret_cast<GPostEventSource *>(source);

    postEventSource->serialNumber.storeRelaxed(1);
    postEventSource->d = this;
    g_source_set_can_recurse(&postEventSource->source, true);
    g_source_attach(&postEventSource->source, mainContext);

    // setup socketNotifierSource
    source = g_source_new(&socketNotifierSourceFuncs, sizeof(GSocketNotifierSource));
    g_source_set_name(source, "[Qt] GSocketNotifierSource");
    socketNotifierSource = reinterpret_cast<GSocketNotifierSource *>(source);
    (void) new (&socketNotifierSource->pollfds) QList<GPollFDWithQSocketNotifier *>();
    g_source_set_can_recurse(&socketNotifierSource->source, true);
    g_source_attach(&socketNotifierSource->source, mainContext);

    // setup normal and idle timer sources
    source = g_source_new(&timerSourceFuncs, sizeof(GTimerSource));
    g_source_set_name(source, "[Qt] GTimerSource");
    timerSource = reinterpret_cast<GTimerSource *>(source);
    (void) new (&timerSource->timerList) QTimerInfoList();
    timerSource->processEventsFlags = QEventLoop::AllEvents;
    timerSource->runWithIdlePriority = false;
    g_source_set_can_recurse(&timerSource->source, true);
    g_source_attach(&timerSource->source, mainContext);

    source = g_source_new(&idleTimerSourceFuncs, sizeof(GIdleTimerSource));
    g_source_set_name(source, "[Qt] GIdleTimerSource");
    idleTimerSource = reinterpret_cast<GIdleTimerSource *>(source);
    idleTimerSource->timerSource = timerSource;
    g_source_set_can_recurse(&idleTimerSource->source, true);
    g_source_attach(&idleTimerSource->source, mainContext);
}

void QEventDispatcherGlibPrivate::runTimersOnceWithNormalPriority()
{
    timerSource->runWithIdlePriority = false;
}

QEventDispatcherGlib::QEventDispatcherGlib(QObject *parent)
    : QAbstractEventDispatcher(*(new QEventDispatcherGlibPrivate), parent)
{
}

QEventDispatcherGlib::QEventDispatcherGlib(GMainContext *mainContext, QObject *parent)
    : QAbstractEventDispatcher(*(new QEventDispatcherGlibPrivate(mainContext)), parent)
{ }

QEventDispatcherGlib::~QEventDispatcherGlib()
{
    Q_D(QEventDispatcherGlib);

    // destroy all timer sources
    qDeleteAll(d->timerSource->timerList);
    d->timerSource->timerList.~QTimerInfoList();
    g_source_destroy(&d->timerSource->source);
    g_source_unref(&d->timerSource->source);
    d->timerSource = nullptr;
    g_source_destroy(&d->idleTimerSource->source);
    g_source_unref(&d->idleTimerSource->source);
    d->idleTimerSource = nullptr;

    // destroy socket notifier source
    for (int i = 0; i < d->socketNotifierSource->pollfds.count(); ++i) {
        GPollFDWithQSocketNotifier *p = d->socketNotifierSource->pollfds[i];
        g_source_remove_poll(&d->socketNotifierSource->source, &p->pollfd);
        delete p;
    }
    d->socketNotifierSource->pollfds.~QList<GPollFDWithQSocketNotifier *>();
    g_source_destroy(&d->socketNotifierSource->source);
    g_source_unref(&d->socketNotifierSource->source);
    d->socketNotifierSource = nullptr;

    // destroy post event source
    g_source_destroy(&d->postEventSource->source);
    g_source_unref(&d->postEventSource->source);
    d->postEventSource = nullptr;

    Q_ASSERT(d->mainContext != nullptr);
#if GLIB_CHECK_VERSION (2, 22, 0)
    g_main_context_pop_thread_default (d->mainContext);
#endif
    g_main_context_unref(d->mainContext);
    d->mainContext = nullptr;
}

bool QEventDispatcherGlib::processEvents(QEventLoop::ProcessEventsFlags flags)
{
    Q_D(QEventDispatcherGlib);

    const bool canWait = (flags & QEventLoop::WaitForMoreEvents);
    if (canWait)
        emit aboutToBlock();
    else
        emit awake();

    // tell postEventSourcePrepare() and timerSource about any new flags
    QEventLoop::ProcessEventsFlags savedFlags = d->timerSource->processEventsFlags;
    d->timerSource->processEventsFlags = flags;

    if (!(flags & QEventLoop::EventLoopExec)) {
        // force timers to be sent at normal priority
        d->timerSource->runWithIdlePriority = false;
    }

    // start processing all priorities
    bool result = qt_g_main_context_iteration(d->mainContext, canWait, true);
    while (!result && canWait)
        result = g_main_context_iteration(d->mainContext, canWait);

    d->timerSource->processEventsFlags = savedFlags;

    if (canWait)
        emit awake();

    return result;
}

void QEventDispatcherGlib::registerSocketNotifier(QSocketNotifier *notifier)
{
    Q_ASSERT(notifier);
    int sockfd = notifier->socket();
    int type = notifier->type();
#ifndef QT_NO_DEBUG
    if (sockfd < 0) {
        qWarning("QSocketNotifier: Internal error");
        return;
    } else if (notifier->thread() != thread()
               || thread() != QThread::currentThread()) {
        qWarning("QSocketNotifier: socket notifiers cannot be enabled from another thread");
        return;
    }
#endif

    Q_D(QEventDispatcherGlib);


    GPollFDWithQSocketNotifier *p = new GPollFDWithQSocketNotifier;
    p->pollfd.fd = sockfd;
    switch (type) {
    case QSocketNotifier::Read:
        p->pollfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
        break;
    case QSocketNotifier::Write:
        p->pollfd.events = G_IO_OUT | G_IO_ERR;
        break;
    case QSocketNotifier::Exception:
        p->pollfd.events = G_IO_PRI | G_IO_ERR;
        break;
    }
    p->socketNotifier = notifier;

    d->socketNotifierSource->pollfds.append(p);

    g_source_add_poll(&d->socketNotifierSource->source, &p->pollfd);
}

void QEventDispatcherGlib::unregisterSocketNotifier(QSocketNotifier *notifier)
{
    Q_ASSERT(notifier);
#ifndef QT_NO_DEBUG
    int sockfd = notifier->socket();
    if (sockfd < 0) {
        qWarning("QSocketNotifier: Internal error");
        return;
    } else if (notifier->thread() != thread()
               || thread() != QThread::currentThread()) {
        qWarning("QSocketNotifier: socket notifiers cannot be disabled from another thread");
        return;
    }
#endif

    Q_D(QEventDispatcherGlib);

    for (int i = 0; i < d->socketNotifierSource->pollfds.count(); ++i) {
        GPollFDWithQSocketNotifier *p = d->socketNotifierSource->pollfds.at(i);
        if (p->socketNotifier == notifier) {
            // found it
            g_source_remove_poll(&d->socketNotifierSource->source, &p->pollfd);

            d->socketNotifierSource->pollfds.removeAt(i);
            delete p;

            return;
        }
    }
}

void QEventDispatcherGlib::registerTimer(int timerId, qint64 interval, Qt::TimerType timerType, QObject *object)
{
#ifndef QT_NO_DEBUG
    if (timerId < 1 || interval < 0 || !object) {
        qWarning("QEventDispatcherGlib::registerTimer: invalid arguments");
        return;
    } else if (object->thread() != thread() || thread() != QThread::currentThread()) {
        qWarning("QEventDispatcherGlib::registerTimer: timers cannot be started from another thread");
        return;
    }
#endif

    Q_D(QEventDispatcherGlib);
    d->timerSource->timerList.registerTimer(timerId, interval, timerType, object);
}

bool QEventDispatcherGlib::unregisterTimer(int timerId)
{
#ifndef QT_NO_DEBUG
    if (timerId < 1) {
        qWarning("QEventDispatcherGlib::unregisterTimer: invalid argument");
        return false;
    } else if (thread() != QThread::currentThread()) {
        qWarning("QEventDispatcherGlib::unregisterTimer: timers cannot be stopped from another thread");
        return false;
    }
#endif

    Q_D(QEventDispatcherGlib);
    return d->timerSource->timerList.unregisterTimer(timerId);
}

bool QEventDispatcherGlib::unregisterTimers(QObject *object)
{
#ifndef QT_NO_DEBUG
    if (!object) {
        qWarning("QEventDispatcherGlib::unregisterTimers: invalid argument");
        return false;
    } else if (object->thread() != thread() || thread() != QThread::currentThread()) {
        qWarning("QEventDispatcherGlib::unregisterTimers: timers cannot be stopped from another thread");
        return false;
    }
#endif

    Q_D(QEventDispatcherGlib);
    return d->timerSource->timerList.unregisterTimers(object);
}

QList<QEventDispatcherGlib::TimerInfo> QEventDispatcherGlib::registeredTimers(QObject *object) const
{
    if (!object) {
        qWarning("QEventDispatcherUNIX:registeredTimers: invalid argument");
        return QList<TimerInfo>();
    }

    Q_D(const QEventDispatcherGlib);
    return d->timerSource->timerList.registeredTimers(object);
}

int QEventDispatcherGlib::remainingTime(int timerId)
{
#ifndef QT_NO_DEBUG
    if (timerId < 1) {
        qWarning("QEventDispatcherGlib::remainingTimeTime: invalid argument");
        return -1;
    }
#endif

    Q_D(QEventDispatcherGlib);
    return d->timerSource->timerList.timerRemainingTime(timerId);
}

void QEventDispatcherGlib::interrupt()
{
    wakeUp();
}

void QEventDispatcherGlib::wakeUp()
{
    Q_D(QEventDispatcherGlib);
    d->postEventSource->serialNumber.ref();
    g_main_context_wakeup(d->mainContext);
}

bool QEventDispatcherGlib::versionSupported()
{
#if !defined(GLIB_MAJOR_VERSION) || !defined(GLIB_MINOR_VERSION) || !defined(GLIB_MICRO_VERSION)
    return false;
#else
    return ((GLIB_MAJOR_VERSION << 16) + (GLIB_MINOR_VERSION << 8) + GLIB_MICRO_VERSION) >= 0x020301;
#endif
}

QEventDispatcherGlib::QEventDispatcherGlib(QEventDispatcherGlibPrivate &dd, QObject *parent)
    : QAbstractEventDispatcher(dd, parent)
{
}

QT_END_NAMESPACE

// defining glib internal data structures

typedef struct _GPollRec GPollRec;

struct _GPollRec
{
    GPollFD *fd;
    GPollRec *prev;
    GPollRec *next;
    gint priority;
};
typedef struct _GPollRec GPollRec;

typedef struct _GWakeup GWakeup;

struct _GMainContext
{
    /* The following lock is used for both the list of sources
     * and the list of poll records
     */
    GMutex mutex;
    GCond cond;
    GThread *owner;
    guint owner_count;
    GSList *waiters;

    volatile gint ref_count;

    GHashTable *sources;              /* guint -> GSource */

    GPtrArray *pending_dispatches;
    gint timeout;			/* Timeout for current iteration */

    guint next_id;
    GList *source_lists;
    gint in_check_or_prepare;

    GPollRec *poll_records;
    guint n_poll_records;
    GPollFD *cached_poll_array;
    guint cached_poll_array_size;

    GWakeup *wakeup;

    GPollFD wake_up_rec;

/* Flag indicating whether the set of fd's changed during a poll */
    gboolean poll_changed;

    GPollFunc poll_func;

    gint64   time;
    gboolean time_is_fresh;
};

struct _GMainWaiter
{
    GCond *cond;
    GMutex *mutex;
};
typedef struct _GMainWaiter GMainWaiter;

// declaring glib internal functions

static gboolean
qt_g_main_context_iterate (GMainContext *context,
                           gboolean      block,
                           gboolean      dispatch,
                           GThread      *self,
                           gboolean all_priorities);

static void
qt_g_main_context_poll (GMainContext *context,
                        gint          timeout,
                        gint          priority,
                        GPollFD      *fds,
                        gint          n_fds);

// glib internal defines

#define LOCK_CONTEXT(context) g_mutex_lock (&context->mutex)
#define UNLOCK_CONTEXT(context) g_mutex_unlock (&context->mutex)
#define G_TRACE_CURRENT_TIME 0
#define G_THREAD_SELF g_thread_self ()

// redefining glib functions

gboolean
qt_g_main_context_iteration (GMainContext *context, gboolean may_block, gboolean all_priorities)
{
    gboolean retval;

    if (!context)
        context = g_main_context_default();

    LOCK_CONTEXT (context);
    retval = qt_g_main_context_iterate (context, may_block, TRUE, g_thread_self (), all_priorities);
    UNLOCK_CONTEXT (context);

    return retval;
}

static gboolean
qt_g_main_context_wait_internal (GMainContext *context,
                              GCond        *cond,
                              GMutex       *mutex)
{
    gboolean result = FALSE;
    GThread *self = G_THREAD_SELF;
    gboolean loop_internal_waiter;

    if (context == NULL)
        context = g_main_context_default ();

    loop_internal_waiter = (mutex == &context->mutex);

    if (!loop_internal_waiter)
        LOCK_CONTEXT (context);

    if (context->owner && context->owner != self)
    {
        GMainWaiter waiter;

        waiter.cond = cond;
        waiter.mutex = mutex;

        context->waiters = g_slist_append (context->waiters, &waiter);

        if (!loop_internal_waiter)
            UNLOCK_CONTEXT (context);
        g_cond_wait (cond, mutex);
        if (!loop_internal_waiter)
            LOCK_CONTEXT (context);

        context->waiters = g_slist_remove (context->waiters, &waiter);
    }

    if (!context->owner)
    {
        context->owner = self;
        g_assert (context->owner_count == 0);
    }

    if (context->owner == self)
    {
        context->owner_count++;
        result = TRUE;
    }

    if (!loop_internal_waiter)
        UNLOCK_CONTEXT (context);

    return result;
}

static gboolean
qt_g_main_context_iterate (GMainContext *context,
                        gboolean      block,
                        gboolean      dispatch,
                        GThread      *,
                        gboolean all_priorities)
{
    gint max_priority;
    gint timeout;
    gboolean some_ready;
    gint nfds, allocated_nfds;
    GPollFD *fds = NULL;
    gint64 begin_time_nsec G_GNUC_UNUSED;

    UNLOCK_CONTEXT (context);

    begin_time_nsec = G_TRACE_CURRENT_TIME;

    if (!g_main_context_acquire (context))
    {
        gboolean got_ownership;

        LOCK_CONTEXT (context);

        if (!block)
            return FALSE;

        got_ownership = qt_g_main_context_wait_internal (context,
                                                      &context->cond,
                                                      &context->mutex);

        if (!got_ownership)
            return FALSE;
    }
    else
        LOCK_CONTEXT (context);

    if (!context->cached_poll_array)
    {
        context->cached_poll_array_size = context->n_poll_records;
        context->cached_poll_array = g_new (GPollFD, context->n_poll_records);
    }

    allocated_nfds = context->cached_poll_array_size;
    fds = context->cached_poll_array;

    UNLOCK_CONTEXT (context);

    g_main_context_prepare (context, &max_priority);

    // only the following two lines are different from glib implementation
    if (all_priorities)
        max_priority = G_MAXINT;

    while ((nfds = g_main_context_query (context, max_priority, &timeout, fds,
                                         allocated_nfds)) > allocated_nfds)
    {
        LOCK_CONTEXT (context);
        g_free (fds);
        context->cached_poll_array_size = allocated_nfds = nfds;
        context->cached_poll_array = fds = g_new (GPollFD, nfds);
        UNLOCK_CONTEXT (context);
    }

    if (!block)
        timeout = 0;

    qt_g_main_context_poll (context, timeout, max_priority, fds, nfds);

    some_ready = g_main_context_check (context, max_priority, fds, nfds);

    if (dispatch)
        g_main_context_dispatch (context);

    g_main_context_release (context);

    LOCK_CONTEXT (context);

    return some_ready;
}

static void
qt_g_main_context_poll (GMainContext *context,
                     gint          timeout,
                     gint          ,
                     GPollFD      *fds,
                     gint          n_fds)
{
#ifdef  G_MAIN_POLL_DEBUG
    GTimer *poll_timer;
  GPollRec *pollrec;
  gint i;
#endif

    GPollFunc poll_func;

    if (n_fds || timeout != 0)
    {
        int ret, errsv;

#ifdef	G_MAIN_POLL_DEBUG
        poll_timer = NULL;
      if (_g_main_poll_debug)
	{
	  g_print ("polling context=%p n=%d timeout=%d\n",
		   context, n_fds, timeout);
	  poll_timer = g_timer_new ();
	}
#endif

        LOCK_CONTEXT (context);

        poll_func = context->poll_func;

        UNLOCK_CONTEXT (context);
        ret = (*poll_func) (fds, n_fds, timeout);
        errsv = errno;
        if (ret < 0 && errsv != EINTR)
        {
#ifndef G_OS_WIN32
            g_warning ("poll(2) failed due to: %s.",
                       g_strerror (errsv));
#else

#endif
        }

#ifdef	G_MAIN_POLL_DEBUG
        if (_g_main_poll_debug)
	{
	  LOCK_CONTEXT (context);

	  g_print ("g_main_poll(%d) timeout: %d - elapsed %12.10f seconds",
		   n_fds,
		   timeout,
		   g_timer_elapsed (poll_timer, NULL));
	  g_timer_destroy (poll_timer);
	  pollrec = context->poll_records;

	  while (pollrec != NULL)
	    {
	      i = 0;
	      while (i < n_fds)
		{
		  if (fds[i].fd == pollrec->fd->fd &&
		      pollrec->fd->events &&
		      fds[i].revents)
		    {
		      g_print (" [" G_POLLFD_FORMAT " :", fds[i].fd);
		      if (fds[i].revents & G_IO_IN)
			g_print ("i");
		      if (fds[i].revents & G_IO_OUT)
			g_print ("o");
		      if (fds[i].revents & G_IO_PRI)
			g_print ("p");
		      if (fds[i].revents & G_IO_ERR)
			g_print ("e");
		      if (fds[i].revents & G_IO_HUP)
			g_print ("h");
		      if (fds[i].revents & G_IO_NVAL)
			g_print ("n");
		      g_print ("]");
		    }
		  i++;
		}
	      pollrec = pollrec->next;
	    }
	  g_print ("\n");

	  UNLOCK_CONTEXT (context);
	}
#endif
    }
}

#include "moc_qeventdispatcher_glib_p.cpp"
