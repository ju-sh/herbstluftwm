#include "xmainloop.h"

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <sys/wait.h>
#include <iostream>
#include <memory>

#include "client.h"
#include "clientmanager.h"
#include "command.h"
#include "decoration.h"
#include "desktopwindow.h"
#include "ewmh.h"
#include "framedecoration.h"
#include "frametree.h"
#include "hlwmcommon.h"
#include "ipc-server.h"
#include "keymanager.h"
#include "layout.h"
#include "monitor.h"
#include "monitormanager.h"
#include "mousemanager.h"
#include "panelmanager.h"
#include "root.h"
#include "rules.h"
#include "settings.h"
#include "tag.h"
#include "tagmanager.h"
#include "utils.h"
#include "watchers.h"
#include "xconnection.h"

using std::make_pair;
using std::function;
using std::shared_ptr;
using std::string;
using std::vector;

/** A custom event handler casting function.
 *
 * It ensures that the pointer that is casted to the EventHandler
 * type is indeed a member function that accepts one pointer.
 *
 * Note that this is as (much or less) hacky as a member access to the XEvent
 * union type itself.
 */
template<typename T>
inline XMainLoop::EventHandler EH(void (XMainLoop::*handler)(T*)) {
    return (XMainLoop::EventHandler) handler;
}

XMainLoop::XMainLoop(XConnection& X, Root* root)
    : X_(X)
    , root_(root)
    , aboutToQuit_(false)
    , handlerTable_()
{
    handlerTable_[ ButtonPress       ] = EH(&XMainLoop::buttonpress);
    handlerTable_[ ButtonRelease     ] = EH(&XMainLoop::buttonrelease);
    handlerTable_[ ClientMessage     ] = EH(&XMainLoop::clientmessage);
    handlerTable_[ ConfigureNotify   ] = EH(&XMainLoop::configurenotify);
    handlerTable_[ ConfigureRequest  ] = EH(&XMainLoop::configurerequest);
    handlerTable_[ CreateNotify      ] = EH(&XMainLoop::createnotify);
    handlerTable_[ DestroyNotify     ] = EH(&XMainLoop::destroynotify);
    handlerTable_[ EnterNotify       ] = EH(&XMainLoop::enternotify);
    handlerTable_[ Expose            ] = EH(&XMainLoop::expose);
    handlerTable_[ FocusIn           ] = EH(&XMainLoop::focusin);
    handlerTable_[ KeyPress          ] = EH(&XMainLoop::keypress);
    handlerTable_[ MapNotify         ] = EH(&XMainLoop::mapnotify);
    handlerTable_[ MapRequest        ] = EH(&XMainLoop::maprequest);
    handlerTable_[ MappingNotify     ] = EH(&XMainLoop::mappingnotify);
    handlerTable_[ MotionNotify      ] = EH(&XMainLoop::motionnotify);
    handlerTable_[ PropertyNotify    ] = EH(&XMainLoop::propertynotify);
    handlerTable_[ UnmapNotify       ] = EH(&XMainLoop::unmapnotify);
    handlerTable_[ SelectionClear    ] = EH(&XMainLoop::selectionclear);

    // get events from hlwm:
    root_->monitors->dropEnterNotifyEvents
            .connect(this, &XMainLoop::dropEnterNotifyEvents);

    root_->clients->dragged.changed()
            .connect(this, &XMainLoop::draggedClientChanges);
}

//! scan for windows and add them to the list of managed clients
// from dwm.c
void XMainLoop::scanExistingClients() {
    XWindowAttributes wa;
    auto clientmanager = root_->clients();
    auto& initialEwmhState = root_->ewmh_.initialState();
    auto& originalClients = initialEwmhState.original_client_list_;
    auto isInOriginalClients = [&originalClients] (Window win) {
        return originalClients.end()
            != std::find(originalClients.begin(), originalClients.end(), win);
    };
    auto findTagForWindow = [this](Window win) -> function<void(ClientChanges&)> {
            if (!root_->globals.importTagsFromEwmh) {
                // do nothing, if import is disabled
                return [] (ClientChanges&) {};
            }
            return [this,win] (ClientChanges& changes) {
                long idx = this->root_->ewmh_.windowGetInitialDesktop(win);
                if (idx < 0) {
                    return;
                }
                HSTag* tag = root_->tags->byIdx((size_t)idx);
                if (tag) {
                    changes.tag_name = tag->name();
                }
            };
    };
    for (auto win : X_.queryTree(X_.root())) {
        if (!XGetWindowAttributes(X_.display(), win, &wa) || wa.override_redirect)
        {
            continue;
        }
        // only manage mapped windows.. no strange wins like:
        //      luakit/dbus/(ncurses-)vim
        // but manage it if it was in the ewmh property _NET_CLIENT_LIST by
        // the previous window manager
        // TODO: what would dwm do?
        if (root_->ewmh_.isOwnWindow(win)) {
            continue;
        }
        if (root_->ewmh_.getWindowType(win) == NetWmWindowTypeDesktop)
        {
            DesktopWindow::registerDesktop(win);
            root_->monitors->restack();
            XMapWindow(X_.display(), win);
        }
        else if (root_->ewmh_.getWindowType(win) == NetWmWindowTypeDock)
        {
            root_->panels->registerPanel(win);
            XSelectInput(X_.display(), win, PropertyChangeMask);
            XMapWindow(X_.display(), win);
        }
        else if (wa.map_state == IsViewable
            || isInOriginalClients(win)) {
            Client* c = clientmanager->manage_client(win, true, false, findTagForWindow(win));
            if (root_->monitors->byTag(c->tag())) {
                XMapWindow(X_.display(), win);
            }
        }
    }
    // ensure every original client is managed again
    for (auto win : originalClients) {
        if (clientmanager->client(win)) {
            continue;
        }
        if (!XGetWindowAttributes(X_.display(), win, &wa)
            || wa.override_redirect)
        {
            continue;
        }
        XReparentWindow(X_.display(), win, X_.root(), 0,0);
        clientmanager->manage_client(win, true, false, findTagForWindow(win));
    }
    root_->monitors->restack();
}


void XMainLoop::run() {
    XEvent event;
    int x11_fd;
    fd_set in_fds;
    x11_fd = ConnectionNumber(X_.display());
    while (!aboutToQuit_) {
        // before making the process hang in the `select` call,
        // first collect all zombies:
        collectZombies();
        // set the the `select` sets:
        FD_ZERO(&in_fds);
        FD_SET(x11_fd, &in_fds);
        // wait for an event or a signal
        select(x11_fd + 1, &in_fds, nullptr, nullptr, nullptr);
        // if `select` was interrupted by a signal, then it was maybe SIGCHLD
        collectZombies();
        if (aboutToQuit_) {
            break;
        }
        XSync(X_.display(), False);
        while (XQLength(X_.display())) {
            XNextEvent(X_.display(), &event);
            EventHandler handler = handlerTable_[event.type];
            if (handler != nullptr) {
                (this ->* handler)(&event);
            }
            root_->watchers->scanForChanges();
            XSync(X_.display(), False);
        }
    }
}

void XMainLoop::collectZombies()
{
    int childInfo;
    pid_t childPid;
    while (true) {
        childPid = waitpid(-1, &childInfo, WNOHANG);
        if (childPid <= 0) {
            break;
        }
        childExited.emit(make_pair(childPid, WEXITSTATUS(childInfo)));
    }
}

void XMainLoop::quit() {
    aboutToQuit_ = true;
}

void XMainLoop::dropEnterNotifyEvents()
{
    if (duringEnterNotify_) {
        // during a enternotify(), no artificial enter notify events
        // can be created. Moreover, on quick mouse movements, an enter notify
        // might be followed by further enter notify events, which
        // must not be dropped.
        return;
    }
    XEvent ev;
    XSync(X_.display(), False);
    while (XCheckMaskEvent(X_.display(), EnterWindowMask, &ev)) {
    }
}

/* ----------------------------- */
/* event handler implementations */
/* ----------------------------- */

void XMainLoop::buttonpress(XButtonEvent* be) {
    MouseManager* mm = root_->mouse();
    HSDebug("name is: ButtonPress on sub 0x%lx, win 0x%lx\n", be->subwindow, be->window);
    if (!mm->mouse_handle_event(be->state, be->button, be->window)) {
        // if the event was not handled by the mouse manager, pass it to the client:
        Client* client = root_->clients->client(be->window);
        if (!client) {
            client = Decoration::toClient(be->window);
        }
        if (client) {
            Client* tabClient = {};
            if (be->window == client->dec->decorationWindow()
                && be->button == Button1)
            {
                auto maybeClick = client->dec->positionHasButton({be->x, be->y});
                if (maybeClick.has_value()) {
                    tabClient = maybeClick.value().tabClient_;
                }
            }
            bool raise = root_->settings->raise_on_click();
            if (tabClient) {
                focus_client(tabClient, false, true, raise);
            } else {
                focus_client(client, false, true, raise);
                if (be->window == client->decorationWindow()) {
                    ResizeAction resize = client->dec->positionTriggersResize({be->x, be->y});
                    if (resize) {
                        mm->mouse_initiate_resize(client, resize);
                    } else {
                        mm->mouse_initiate_move(client, {});
                    }
                }
            }
        }
    }
    FrameDecoration* frameDec = FrameDecoration::withWindow(be->window);
    if (frameDec) {
        auto frame = frameDec->frame();
        if (frame)  {
            root_->focusFrame(frame);
        }
    }
    XAllowEvents(X_.display(), ReplayPointer, be->time);
}

void XMainLoop::buttonrelease(XButtonEvent*) {
    HSDebug("name is: ButtonRelease\n");
    root_->mouse->mouse_stop_drag();
}

void XMainLoop::createnotify(XCreateWindowEvent* event) {
    // printf("name is: CreateNotify\n");
    if (root_->ipcServer_.isConnectable(event->window)) {
        root_->ipcServer_.addConnection(event->window);
        root_->ipcServer_.handleConnection(event->window,
                                           XMainLoop::callCommand);
    }
}

void XMainLoop::configurerequest(XConfigureRequestEvent* cre) {
    HSDebug("name is: ConfigureRequest for 0x%lx\n", cre->window);
    Client* client = root_->clients->client(cre->window);
    if (client) {
        bool changes = false;
        Rectangle newRect = client->float_size_;
        if (client->sizehints_floating_ &&
            (client->is_client_floated() || client->pseudotile_))
        {
            bool width_requested = 0 != (cre->value_mask & CWWidth);
            bool height_requested = 0 != (cre->value_mask & CWHeight);
            bool x_requested = 0 != (cre->value_mask & CWX);
            bool y_requested = 0 != (cre->value_mask & CWY);
            if (width_requested && newRect.width != cre->width) {
                changes = true;
            }
            if (height_requested && newRect.height != cre->height) {
                changes = true;
            }
            if (x_requested || y_requested) {
                changes = true;
            }
            if (x_requested || y_requested) {
                // if only one of the two dimensions is requested, then just
                // set the other to some reasonable value.
                if (!x_requested) {
                    cre->x = client->last_size_.x;
                }
                if (!y_requested) {
                    cre->y = client->last_size_.y;
                }
                // interpret the x and y coordinate relative to the monitor they are currently on
                Monitor* m = root_->monitors->byTag(client->tag());
                if (!m) {
                    // if the client is not visible at the moment, take the monitor that is
                    // most appropriate according to the requested cooridnates:
                    m = root_->monitors->byCoordinate({cre->x, cre->y});
                }
                if (!m) {
                    // if we have not found a suitable monitor, take the current
                    m = root_->monitors->focus();
                }
                // the requested coordinates are relative to the root window.
                // convert them to coordinates relative to the monitor.
                cre->x -= m->rect->x + *m->pad_left;
                cre->y -= m->rect->y + *m->pad_up;
                newRect.x = cre->x;
                newRect.y = cre->y;
            }
            if (width_requested) {
                newRect.width = cre->width;
            }
            if (height_requested) {
                newRect.height = cre->height;
            }
        }
        if (changes && client->is_client_floated()) {
            client->float_size_ = newRect;
            client->resize_floating(find_monitor_with_tag(client->tag()), client == get_current_client());
        } else if (changes && client->pseudotile_) {
            client->float_size_ = newRect;
            Monitor* m = find_monitor_with_tag(client->tag());
            if (m) {
                m->applyLayout();
            }
        } else {
        // FIXME: why send event and not XConfigureWindow or XMoveResizeWindow??
            client->send_configure(true);
        }
    } else {
        // if client not known.. then allow configure.
        // its probably a nice conky or dzen2 bar :)
        XWindowChanges wc;
        wc.x = cre->x;
        wc.y = cre->y;
        wc.width = cre->width;
        wc.height = cre->height;
        wc.border_width = cre->border_width;
        wc.sibling = cre->above;
        wc.stack_mode = cre->detail;
        XConfigureWindow(X_.display(), cre->window, cre->value_mask, &wc);
    }
}

void XMainLoop::clientmessage(XClientMessageEvent* event) {
    root_->ewmh_.handleClientMessage(event);
}

void XMainLoop::configurenotify(XConfigureEvent* event) {
    if (event->window == X_.root()) {
        root_->panels->rootWindowChanged(event->width, event->height);
        if (root_->settings->auto_detect_monitors()) {
            Input input = Input("detect_monitors");
            std::ostringstream void_output;
            // discard output, but forward errors to std:cerr
            OutputChannels channels("", void_output, std::cerr);
            root_->monitors->detectMonitorsCommand(input, channels);
        }
    } else {
        Rectangle geometry = { event->x, event->y, event->width, event->height };
        root_->panels->geometryChanged(event->window, geometry);
    }
    // HSDebug("name is: ConfigureNotify\n");
}

void XMainLoop::destroynotify(XUnmapEvent* event) {
    // try to unmanage it
    //HSDebug("name is: DestroyNotify for %lx\n", event->xdestroywindow.window);
    auto cm = root_->clients();
    auto client = cm->client(event->window);
    if (client) {
        cm->force_unmanage(client);
    } else {
        DesktopWindow::unregisterDesktop(event->window);
        root_->panels->unregisterPanel(event->window);
    }
}

void XMainLoop::enternotify(XCrossingEvent* ce) {
    HSDebug("name is: EnterNotify, focus = %d, window = 0x%lx\n", ce->focus, ce->window);
    if (ce->mode != NotifyNormal || ce->detail == NotifyInferior) {
        // ignore an event if it is caused by (un-)grabbing the mouse or
        // if the pointer moves from a window to its decoration.
        // for 'ce->detail' see:
        // https://tronche.com/gui/x/xlib/events/window-entry-exit/normal.html
        return;
    }
    // Warning: we have to set this to false again!
    duringEnterNotify_ = true;
    Client* decorationClient = Decoration::toClient(ce->window);
    if (decorationClient) {
        decorationClient->dec->updateResizeAreaCursors();
    }
    if (!root_->mouse->mouse_is_dragging()
        && root_->settings()->focus_follows_mouse()
        && ce->focus == false) {
        Client* c = root_->clients->client(ce->window);
        if (!c) {
            c = decorationClient;
        }
        shared_ptr<FrameLeaf> target;
        if (c && c->tag()->floating == false
              && (target = c->tag()->frame->root_->frameWithClient(c))
              && target->getLayout() == LayoutAlgorithm::max
              && target->focusedClient() != c) {
            // don't allow focus_follows_mouse if another window would be
            // hidden during that focus change (which only occurs in max layout)
        } else if (c) {
            focus_client(c, false, true, false);
        }
        if (!c) {
            // if it's not a client window, it's maybe a frame
            FrameDecoration* frameDec = FrameDecoration::withWindow(ce->window);
            if (frameDec) {
                auto frame = frameDec->frame();
                HSWeakAssert(frame);
                if (frame) {
                    root_->focusFrame(frame);
                }
            }
        }
    }
    duringEnterNotify_ = false;
}

void XMainLoop::expose(XEvent* event) {
    //if (event->xexpose.count > 0) return;
    //Window ewin = event->xexpose.window;
    //HSDebug("name is: Expose for window %lx\n", ewin);
}

void XMainLoop::focusin(XFocusChangeEvent* event) {
    // get the newest FocusIn event, otherwise we could trigger
    // an endless loop of FocusIn events
    while (XCheckMaskEvent(X_.display(), FocusChangeMask, (XEvent *)event)) {
        ;
    }
    HSDebug("FocusIn for 0x%lx (%s)\n",
            event->window,
            XConnection::focusChangedDetailToString(event->detail));
    if (event->type == FocusIn
        && (event->detail == NotifyNonlinear
            || event->detail == NotifyNonlinearVirtual))
    {
        // an event if an application steals input focus
        // directly via XSetInputFocus, e.g. via `xdotool windowfocus`.
        // also other applications do this, e.g. `emacsclient -n FILENAME`
        // when an emacs window exist. There are still subtle differences between
        // xdotool and emacsclient: xdotool generates detail=NotifyNonlinear
        // whereas emacsclient only detail=NotifyNonlinearVirtual.
        // I don't know how to prevent the keyboard input steal, so all we can
        // do is to update clients.focus accordingly.
        Window currentFocus = 0;
        if (root_->clients->focus()) {
            currentFocus = root_->clients->focus()->x11Window();
        }
        if (event->window != currentFocus) {
            HSDebug("Window 0x%lx steals the focus\n", event->window);
            Client* target = root_->clients->client(event->window);
            // Warning: focus_client() itself calls XSetInputFocus() which might
            // cause an endless loop if we didn't correctly cleared the
            // event queue with XCheckMaskEvent() above!
            focus_client(target, false, true, false);
        }
    }
}

void XMainLoop::keypress(XKeyEvent* event) {
    //HSDebug("name is: KeyPress\n");
    root_->keys()->handleKeyPress(event);
}

void XMainLoop::mappingnotify(XMappingEvent* ev) {
    // regrab when keyboard map changes
    XRefreshKeyboardMapping(ev);
    if(ev->request == MappingKeyboard) {
        root_->keys()->regrabAll();
        //TODO: mouse_regrab_all();
    }
}

void XMainLoop::motionnotify(XMotionEvent* event) {
    // get newest motion notification
    while (XCheckMaskEvent(X_.display(), ButtonMotionMask, (XEvent *)event)) {
        ;
    }
    Point2D newCursorPos = { event->x_root,  event->y_root };
    root_->mouse->handle_motion_event(newCursorPos);
}

void XMainLoop::mapnotify(XMapEvent* event) {
    //HSDebug("name is: MapNotify\n");
    Client* c = root_->clients()->client(event->window);
    if (c != nullptr) {
        // reset focus. so a new window gets the focus if it shall have the
        // input focus
        if (c == root_->clients->focus()) {
            XSetInputFocus(X_.display(), c->window_, RevertToPointerRoot, CurrentTime);
        }
        // also update the window title - just to be sure
        c->update_title();
    } else if (!root_->ewmh_.isOwnWindow(event->window)
               && !Decoration::toClient(event->window)
               && !is_herbstluft_window(X_.display(), event->window)) {
        // the window is not managed.
        HSDebug("MapNotify: briefly managing 0x%lx to apply rules\n", event->window);
        root_->clients()->manage_client(event->window, true, true);
    }
}

void XMainLoop::maprequest(XMapRequestEvent* mapreq) {
    HSDebug("name is: MapRequest for 0x%lx\n", mapreq->window);
    Window window = mapreq->window;
    Client* c = root_->clients()->client(window);
    if (root_->ewmh_.isOwnWindow(window)
        || is_herbstluft_window(X_.display(), window))
    {
        // just map the window if it wants that
        XWindowAttributes wa;
        if (!XGetWindowAttributes(X_.display(), window, &wa)) {
            return;
        }
        XMapWindow(X_.display(), window);
    } else if (c != nullptr) {
        // a maprequest of a managed window means that
        // the window wants to be un-minimized according to
        // the item "Iconic -> Normal" in
        // ICCCM 4.1.4 https://tronche.com/gui/x/icccm/sec-4.html#s-4.1.3.1
        c->minimized_ = false;
    } else {
        // c = nullptr, so the window is not yet managed.
        if (root_->ewmh_.getWindowType(window) == NetWmWindowTypeDesktop)
        {
            DesktopWindow::registerDesktop(window);
            root_->monitors->restack();
            XMapWindow(X_.display(), window);
        }
        else if (root_->ewmh_.getWindowType(window) == NetWmWindowTypeDock)
        {
            root_->panels->registerPanel(window);
            XSelectInput(X_.display(), window, PropertyChangeMask);
            XMapWindow(X_.display(), window);
        } else {
            // client should be managed (is not ignored)
            // but is not managed yet
            auto clientmanager = root_->clients();
            auto client = clientmanager->manage_client(window, false, false);
            if (client && find_monitor_with_tag(client->tag())) {
                XMapWindow(X_.display(), window);
            }
        }
    }
}

void XMainLoop::selectionclear(XSelectionClearEvent* event)
{
    if (event->selection == root_->ewmh_.windowManagerSelection()
        && event->window == root_->ewmh_.windowManagerWindow())
    {
        HSDebug("Getting replaced by another window manager. exiting.");
        quit();
    }
}

void XMainLoop::propertynotify(XPropertyEvent* ev) {
    // printf("name is: PropertyNotify\n");
    Client* client = root_->clients->client(ev->window);
    if (ev->state == PropertyNewValue) {
        if (root_->ipcServer_.isConnectable(ev->window)) {
            root_->ipcServer_.handleConnection(ev->window,
                                               XMainLoop::callCommand);
        } else if (client != nullptr) {
            //char* atomname = XGetAtomName(X_.display(), ev->atom);
            //HSDebug("Property notify for client %s: atom %d \"%s\"\n",
            //        client->window_id_str().c_str(),
            //        ev->atom,
            //        atomname);
            if (ev->atom == XA_WM_HINTS) {
                client->update_wm_hints();
            } else if (ev->atom == XA_WM_NORMAL_HINTS) {
                client->updatesizehints();
                Rectangle geom = client->float_size_;
                client->applysizehints(&geom.width, &geom.height, true);
                client->float_size_ = geom;
                Monitor* m = find_monitor_with_tag(client->tag());
                if (m) {
                    m->applyLayout();
                }
            } else if (ev->atom == XA_WM_NAME ||
                       ev->atom == root_->ewmh_.netatom(NetWmName)) {
                client->update_title();
            } else if (ev->atom == XA_WM_CLASS && client) {
                // according to the ICCCM specification, the WM_CLASS property may only
                // be changed in the withdrawn state:
                // https://www.x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html#wm_class_property
                // If a client violates this, then the window rules like class=... etc are not applied.
                // As a workaround, we do it now:
                auto stdio = OutputChannels::stdio();
                root_->clients()->applyRules(client, stdio);
            }
        } else {
            root_->panels->propertyChanged(ev->window, ev->atom);
        }
    }
}

void XMainLoop::unmapnotify(XUnmapEvent* event) {
    HSDebug("name is: UnmapNotify for window=0x%lx and event=0x%lx\n", event->window, event->event);
    if (event->window == event->event) {
        // reparenting the window creates multiple unmap notify events,
        // both for the root window and the window itself.
        root_->clients()->unmap_notify(event->window);
    }
    if (event->send_event) {
        // if the event was synthetic, then we need to understand it as a kind request
        // by the window to be unmanaged. I don't understand fully how this is implied
        // by the ICCCM documentation:
        // https://tronche.com/gui/x/icccm/sec-4.html#s-4.1.4
        //
        // Anyway, we need to do the following because when running
        // "telegram-desktop -startintray", a window flashes and only
        // sends a synthetic UnmapNotify. So we unmanage the window here
        // to forcefully make the window dissappear.
        XUnmapWindow(X_.display(), event->window);
    }
    // drop all enternotify events
    XSync(X_.display(), False);
    XEvent ev;
    while (XCheckMaskEvent(X_.display(), EnterWindowMask, &ev)) {
        ;
    }
}

IpcServer::CallResult XMainLoop::callCommand(const vector<string>& call)
{
    // the call consists of the command and its arguments
    std::ostringstream output;
    std::ostringstream error;
    string commandName = (call.empty()) ? "" : call[0];
    auto input =
        (call.empty())
        ? Input("", call)
        : Input(call[0], vector<string>(call.begin() + 1, call.end()));
    IpcServer::CallResult result;
    OutputChannels channels(commandName, output, error);
    result.exitCode = Commands::call(input, channels);
    result.output = output.str();
    result.error = error.str();
    return result;
}

void XMainLoop::draggedClientChanges(Client* draggedClient)
{
    if (draggedClient) {
        ResizeAction ra = root_->mouse->resizeAction();
        auto maybeCursor = ra.toCursorShape();
        Cursor cursorShape = XCreateFontCursor(X_.display(), maybeCursor.value_or(XC_fleur));
        // listen for mouse motion events:
        XGrabPointer(X_.display(), draggedClient->x11Window(), True,
            PointerMotionMask|ButtonReleaseMask, GrabModeAsync,
                GrabModeAsync, None, cursorShape, CurrentTime);
    } else { // no client is dragged -> ungrab and clear the queue
        XUngrabPointer(X_.display(), CurrentTime);
        // remove all enternotify-events from the event queue that were
        // generated by the XUngrabPointer
        XEvent ev;
        XSync(X_.display(), False);
        while (XCheckMaskEvent(X_.display(), EnterWindowMask, &ev)) {
        }
    }
}

