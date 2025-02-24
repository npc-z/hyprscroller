//#define COLORS_IPC

#include <hyprland/src/desktop/Window.hpp>
//#include <hyprland/src/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/debug/Log.hpp>
#include <hyprland/src/helpers/Vector2D.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#ifdef COLORS_IPC
#include <hyprland/src/managers/EventManager.hpp>
#endif

#include "scroller.h"
#include <unordered_map>

extern HANDLE PHANDLE;

struct Box {
    Box() : x(0), y(0), w(0), h(0) {}
    Box(double x_, double y_, double w_, double h_)
        : x(x_), y(y_), w(w_), h(h_) {}
    Box(Vector2D pos, Vector2D size)
        : x(pos.x), y(pos.y), w(size.x), h(size.y) {}
    Box(const Box &box)
        : x(box.x), y(box.y), w(box.w), h(box.h) {}

    void set_size(double w_, double h_) {
        w = w_;
        h = h_;
    }
    void set_pos(double x_, double y_) {
        x = x_;
        y = y_;
    }

    double x, y, w, h;
};

enum class ColumnWidth {
    OneThird = 0,
    OneHalf,
    TwoThirds,
    Number,
    Free
};

enum class WindowHeight {
    OneThird,
    OneHalf,
    TwoThirds,
    One,
    Number,
    Free
};

enum class Reorder {
    Auto,
    Lazy
};


class Marks {
public:
    Marks() {}
    ~Marks() { reset(); }
    void reset() {
        marks.clear();
    }
    // Add a mark with name for window, overwriting any existing one with that name
    void add(CWindow *window, const std::string &name) {
        const auto mark = marks.find(name);
        if (mark != marks.end()) {
            mark->second = window;
            return;
        }
        marks[name] = window;
    }
    void del(const std::string &name) {
        const auto mark = marks.find(name);
        if (mark != marks.end()) {
            marks.erase(mark);
        }
    }
    // Remove window from list of marks (used when a window gets deleted)
    void remove(CWindow *window) {
        for(auto it = marks.begin(); it != marks.end();) {
            if (it->second == window)
                it = marks.erase(it);
            else
                it++;
        }
    }
    // If the mark exists, returns that window, otherwise it returns null
    CWindow *visit(const std::string &name) {
        const auto mark = marks.find(name);
        if (mark != marks.end()) {
            return mark->second;
        }
        return nullptr;
    }

private:
    std::unordered_map<std::string, CWindow *> marks;
};

static Marks marks;

class Window {
public:
    Window(CWindow *window, double box_h) : window(window), height(WindowHeight::One), box_h(box_h) {}
    CWindow *ptr() { return window; }
    double get_geom_h() const { return box_h; }
    void set_geom_h(double geom_h) { box_h = geom_h; }
    void push_geom() {
        mem.box_h = box_h;
        mem.pos_y = window->m_vPosition.y;
    }
    void pop_geom() {
        box_h = mem.box_h;
        window->m_vPosition.y = mem.pos_y;
    }
    WindowHeight get_height() const { return height; }
    void update_height(WindowHeight h, double max) {
        height = h;
        switch (height) {
        case WindowHeight::One:
            box_h = max;
            break;
        case WindowHeight::TwoThirds:
            box_h = 2.0 * max / 3.0;
            break;
        case WindowHeight::OneHalf:
            box_h = 0.5 * max;
            break;
        case WindowHeight::OneThird:
            box_h = max / 3.0;
            break;
        default:
            break;
        }
    }
    void set_height_free() { height = WindowHeight::Free; }
private:
    struct Memory {
        double pos_y;
        double box_h;
    };
    CWindow *window;
    WindowHeight height;
    double box_h;
    Memory mem;    // memory to store old height and win y when in maximized/overview modes
};

class Column {
public:
    Column(CWindow *cwindow, double maxw, double maxh)
        : height(WindowHeight::One), reorder(Reorder::Auto), initialized(false), maxdim(false) {
        static auto const *column_default_width = (Hyprlang::STRING const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:column_default_width")->getDataStaticPtr();
        std::string column_width = *column_default_width;
        if (column_width == "onehalf") {
            width = ColumnWidth::OneHalf;
        } else if (column_width == "onethird") {
            width = ColumnWidth::OneThird;
        } else if (column_width == "twothirds") {
            width = ColumnWidth::TwoThirds;
        } else if (column_width == "maximized") {
            width = ColumnWidth::Free;
        } else if (column_width == "floating") {
            if (cwindow->m_vLastFloatingSize.y > 0) {
                width = ColumnWidth::Free;
                maxw = cwindow->m_vLastFloatingSize.x;
            } else {
                width = ColumnWidth::OneHalf;
            }
        } else {
            width = ColumnWidth::OneHalf;
        }
        Window *window = new Window(cwindow, maxh);
        update_width(width, maxw, maxh);
        geom.h = maxh;
        windows.push_back(window);
        active = windows.first();
    }
    Column(Window *window, ColumnWidth width, double maxw, double maxh)
        : width(width), height(WindowHeight::One), reorder(Reorder::Auto), initialized(true), maxdim(false) {
        window->set_geom_h(maxh);
        update_width(width, maxw, maxh);
        geom.h = maxh;
        windows.push_back(window);
        active = windows.first();
    }
    ~Column() {
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            delete win->data();
        }
        windows.clear();
    }
    bool get_init() const { return initialized; }
    void set_init() { initialized = true; }
    size_t size() {
        return windows.size();
    }
    bool has_window(CWindow *window) const {
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            if (win->data()->ptr() == window)
                return true;
        }
        return false;
    }
    void add_active_window(CWindow *window, double maxh) {
        reorder = Reorder::Auto;
        active = windows.emplace_after(active, new Window(window, maxh));
    }
    void remove_window(CWindow *window) {
        reorder = Reorder::Auto;
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            if (win->data()->ptr() == window) {
                if (window == active->data()->ptr()) {
                    // Make next window active (like PaperWM)
                    // If it is the last, make the previous one active.
                    // If it is the only window. active will point to nullptr,
                    // but it doesn't matter because the caller will delete
                    // the column.
                    active = active != windows.last() ? active->next() : active->prev();
                }
                windows.erase(win);
                return;
            }
        }
    }
    void focus_window(CWindow *window) {
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            if (win ->data()->ptr() == window) {
                active = win;
                return;
            }
        }
    }
    double get_geom_x() const {
        return geom.x;
    }
    double get_geom_w() const {
        return geom.w;
    }
    // Used by Row::fit_width()
    void set_geom_w(double w) {
        geom.w = w;
    }
    Vector2D get_height() const {
        Vector2D height;
        CWindow *first = windows.first()->data()->ptr();
        CWindow *last = windows.last()->data()->ptr();
        height.x = first->m_vPosition.y - first->getRealBorderSize();
        height.y = last->m_vPosition.y + last->m_vSize.y + last->getRealBorderSize();
        return height;
    }
    void scale(const Vector2D &bmin, const Vector2D &start, double scale, double gap) {
        for (auto win = windows.first(); win != nullptr; win = win->next()) {
            win->data()->set_geom_h(win->data()->get_geom_h() * scale);
            CWindow *window = win->data()->ptr();
            auto border = window->getRealBorderSize();
            auto gap0 = win == windows.first() ? 0.0 : gap;
            window->m_vPosition = start + Vector2D(border, border) + (window->m_vPosition - Vector2D(border, border) - bmin) * scale;
            window->m_vPosition.y += gap0;
            auto gap1 = win == windows.last() ? 0.0 : gap;
            window->m_vSize.x *= scale;
            window->m_vSize.y = (window->m_vSize.y + 2.0 * border + gap0 + gap1) * scale - gap0 - gap1 - 2.0 * border;
            window->m_vSize = Vector2D(std::max(window->m_vSize.x, 1.0), std::max(window->m_vSize.y, 1.0));
            window->m_vRealSize = window->m_vSize;
            window->m_vRealPosition = window->m_vPosition;
        }
    }
    bool toggle_fullscreen(const Box &fullbbox) {
        CWindow *wactive = active->data()->ptr();
        wactive->m_bIsFullscreen = !wactive->m_bIsFullscreen;
        if (wactive->m_bIsFullscreen) {
            full = fullbbox;
        }
        return wactive->m_bIsFullscreen;
    }
    // Sets fullscreen even if the active window is not full screen
    // Used in recalculateMonitor
    void set_fullscreen(const Box &fullbbox) {
        // Leave it like this (without enabling full screen in the window).
        // If this is called, it won't work unless the window is also set to full screen
        full = fullbbox;
    }
    void push_geom() {
        mem.geom = geom;
        for (auto w = windows.first(); w != nullptr; w = w->next()) {
            w->data()->push_geom();
        }
    }
    void pop_geom() {
        geom = mem.geom;
        for (auto w = windows.first(); w != nullptr; w = w->next()) {
            w->data()->pop_geom();
        }
    }
    void toggle_maximized(double maxw, double maxh) {
        maxdim = !maxdim;
        if (maxdim) {
            mem.geom = geom;
            active->data()->push_geom();
            geom.w = maxw;
            active->data()->set_geom_h(maxh);
        } else {
            geom = mem.geom;
            active->data()->pop_geom();
        }
    }
    bool fullscreen() const {
        return active->data()->ptr()->m_bIsFullscreen;
    }
    bool maximized() const {
        return maxdim;
    }
    // Used by auto-centering of columns
    void set_geom_pos(double x, double y) {
        geom.set_pos(x, y);
    }
    // Recalculates the geometry of the windows in the column
    void recalculate_col_geometry(const Vector2D &gap_x, double gap) {
        if (fullscreen()) {
            active->data()->ptr()->m_vPosition = Vector2D(full.x, full.y);
            active->data()->ptr()->m_vSize = Vector2D(full.w, full.h);
            active->data()->ptr()->m_vRealPosition = active->data()->ptr()->m_vPosition;
            active->data()->ptr()->m_vRealSize = active->data()->ptr()->m_vSize;
        } else {
            // In theory, every window in the Columm should have the same size,
            // but the standard layouts don't follow this rule (to make the code
            // simpler?). Windows close to the border of the monitor will have
            // their sizes affected by gaps_out vs. gaps_in.
            // I follow the same rules.
            // Each window has a gap to its bounding box of "gaps_in + border",
            // except on the monitor sides, where the gap is "gaps_out + border",
            // but the window sizes are different because of those different
            // gaps. So the distance between two window border boundaries is
            // two times gaps_in (one per window).
            Window *wactive = active->data();
            CWindow *win = wactive->ptr();
            auto gap0 = active == windows.first() ? 0.0 : gap;
            auto gap1 = active == windows.last() ? 0.0 : gap;
            auto border = win->getRealBorderSize();
            auto a_y0 = std::round(win->m_vPosition.y - border - gap0);
            auto a_y1 = std::round(win->m_vPosition.y - border - gap0 + wactive->get_geom_h());
            if (a_y0 < geom.y) {
                // active starts above, set it on the top edge
                win->m_vPosition = Vector2D(geom.x + border + gap_x.x, geom.y + border + gap0);
            } else if (a_y1 > geom.y + geom.h) {
                // active overflows below the bottom, move to bottom of viewport
                win->m_vPosition = Vector2D(geom.x + border + gap_x.x, geom.y + geom.h - wactive->get_geom_h() + border + gap0);
            } else {
                // active window is inside the viewport
                if (reorder == Reorder::Auto) {
                    // The active window should always be completely in the viewport.
                    // If any of the windows next to it, above or below are already
                    // in the viewport, keep the current position.
                    bool keep_current = false;
                    if (active->prev() != nullptr) {
                        Window *prev = active->prev()->data();
                        auto gap0 = active->prev() == windows.first() ? 0.0 : gap;
                        auto border = prev->ptr()->getRealBorderSize();
                        auto p_y0 = std::round(prev->ptr()->m_vPosition.y - border - gap0);
                        auto p_y1 = std::round(prev->ptr()->m_vPosition.y - border - gap0 + prev->get_geom_h());
                        if (p_y0 >= geom.y && p_y1 <= geom.y + geom.h) {
                            keep_current = true;
                        }
                    }
                    if (!keep_current && active->next() != nullptr) {
                        Window *next = active->next()->data();
                        auto gap0 = active->next() == windows.first() ? 0.0 : gap;
                        auto border = next->ptr()->getRealBorderSize();
                        auto p_y0 = std::round(next->ptr()->m_vPosition.y - border - gap0);
                        auto p_y1 = std::round(next->ptr()->m_vPosition.y - border - gap0 + next->get_geom_h());
                        if (p_y0 >= geom.y && p_y1 <= geom.y + geom.h) {
                            keep_current = true;
                        }
                    }
                    if (!keep_current) {
                        // If not:
                        // We try to fit the window right below it if it fits
                        // completely, otherwise the one above it. If none of them fit,
                        // we leave it as it is.
                        if (active->next() != nullptr) {
                            if (wactive->get_geom_h() + active->next()->data()->get_geom_h() <= geom.h) {
                                // set next at the bottom edge of the viewport
                                win->m_vPosition = Vector2D(geom.x + border + gap_x.x, geom.y + geom.h - wactive->get_geom_h() - active->next()->data()->get_geom_h() + border + gap0);
                            } else if (active->prev() != nullptr) {
                                if (active->prev()->data()->get_geom_h() + wactive->get_geom_h() <= geom.h) {
                                    // set previous at the top edge of the viewport
                                    win->m_vPosition = Vector2D(geom.x + border + gap_x.x, geom.y + active->prev()->data()->get_geom_h() + border + gap0);
                                } else {
                                    // none of them fit, leave active as it is (only modify x)
                                    win->m_vPosition.x = geom.x + border + gap_x.x;
                                }
                            } else {
                                // nothing above, move active to top of viewport
                                win->m_vPosition = Vector2D(geom.x + border + gap_x.x, geom.y + border + gap0);
                            }
                        } else if (active->prev() != nullptr) {
                            if (active->prev()->data()->get_geom_h() + wactive->get_geom_h() <= geom.h) {
                                // set previous at the top edge of the viewport
                                win->m_vPosition = Vector2D(geom.x + border + gap_x.x, geom.y + active->prev()->data()->get_geom_h() + border + gap0);
                            } else {
                                // it doesn't fit and nothing above, move active to bottom of viewport
                                win->m_vPosition = Vector2D(geom.x + border + gap_x.x, geom.y + geom.h - wactive->get_geom_h() + border + gap0);
                            }
                        } else {
                            // nothing on the right or left, the window is in a correct position
                            win->m_vPosition.x = geom.x + border + gap_x.x;
                        }
                    } else {
                        // the window is in a correct position
                        win->m_vPosition.x = geom.x + border + gap_x.x;
                    }
                } else {
                    // the window is in a correct position
                    win->m_vPosition.x = geom.x + border + gap_x.x;
                }
            }
            adjust_windows(active, gap_x, gap);
        }
    }
    CWindow *get_active_window() {
        return active->data()->ptr();
    }
    void move_active_up() {
        if (active == windows.first())
            return;

        reorder = Reorder::Auto;
        auto prev = active->prev();
        windows.swap(active, prev);
    }
    void move_active_down() {
        if (active == windows.last())
            return;

        reorder = Reorder::Auto;
        auto next = active->next();
        windows.swap(active, next);
    }
    bool move_focus_up(bool focus_wrap) {
        if (active == windows.first()) {
            CMonitor *monitor = g_pCompositor->getMonitorInDirection('u');
            if (monitor == nullptr) {
                if (focus_wrap)
                    active = windows.last();
                return true;
            }
            // use default dispatch for movefocus (change monitor)
            g_pKeybindManager->m_mDispatchers["movefocus"]("u");
            return false;
        }
        reorder = Reorder::Auto;
        active = active->prev();
        return true;
    }
    bool move_focus_down(bool focus_wrap) {
        if (active == windows.last()) {
            CMonitor *monitor = g_pCompositor->getMonitorInDirection('d');
            if (monitor == nullptr) {
                if (focus_wrap)
                    active = windows.first();
                return true;
            }
            // use default dispatch for movefocus (change monitor)
            g_pKeybindManager->m_mDispatchers["movefocus"]("d");
            return false;
        }
        reorder = Reorder::Auto;
        active = active->next();
        return true;
    }
    void admit_window(Window *window) {
        reorder = Reorder::Auto;
        active = windows.emplace_after(active, window);
    }

    Window *expel_active(double gap) {
        reorder = Reorder::Auto;
        Window *window = active->data();
        auto act = active == windows.first() ? active->next() : active->prev();
        windows.erase(active);
        active = act;
        return window;
    }
    void align_window(Direction direction, double gap) {
        CWindow *window = active->data()->ptr();
        auto border = window->getRealBorderSize();
        auto gap0 = active == windows.first() ? 0.0 : gap;
        auto gap1 = active == windows.last() ? 0.0 : gap;
        switch (direction) {
        case Direction::Up:
            reorder = Reorder::Lazy;
            window->m_vPosition.y = geom.y + border + gap0;
            break;
        case Direction::Down:
            reorder = Reorder::Lazy;
            window->m_vPosition.y = geom.y + geom.h - border + gap1;
            break;
        case Direction::Center:
            reorder = Reorder::Lazy;
            window->m_vPosition.y = 0.5 * (geom.y + geom.h - (2.0 * border + gap0 + gap1 + window->m_vSize.y));
            break;
        default:
            break;
        }
    }
    ColumnWidth get_width() const {
        return width;
    }
    // used by Row::fit_width()
    void set_width_free() {
        width = ColumnWidth::Free;
    }
#ifdef COLORS_IPC
    // For IPC events
    std::string get_width_name() const {
        switch (width) {
        case ColumnWidth::OneThird:
            return "OneThird";
        case ColumnWidth::OneHalf:
            return "OneHalf";
        case ColumnWidth::TwoThirds:
            return "TwoThirds";
        case ColumnWidth::Free:
            return "Free";
        default:
            return "";
        }
    }
    std::string get_height_name() const {
        switch (height) {
        case WindowHeight::Auto:
            return "Auto";
        case WindowHeight::Free:
            return "Free";
        default:
            return "";
        }
    }
#endif
    void update_width(ColumnWidth cwidth, double maxw, double maxh) {
        if (maximized()) {
            geom.w = maxw;
        } else {
            switch (cwidth) {
            case ColumnWidth::OneThird:
                geom.w = maxw / 3.0;
                break;
            case ColumnWidth::OneHalf:
                geom.w = maxw / 2.0;
                break;
            case ColumnWidth::TwoThirds:
                geom.w = 2.0 * maxw / 3.0;
                break;
            case ColumnWidth::Free:
                // Only used when creating a column from an expelled window
                geom.w = maxw;
            default:
                break;
            }
        }
        geom.h = maxh;
        width = cwidth;
    }
    void fit_size(FitSize fitsize, const Vector2D &gap_x, double gap) {
        reorder = Reorder::Auto;
        ListNode<Window *> *from, *to;
        switch (fitsize) {
        case FitSize::Active:
            from = to = active;
            break;
        case FitSize::Visible:
            for (auto w = windows.first(); w != nullptr; w = w->next()) {
                auto gap0 = w == windows.first() ? 0.0 : gap;
                auto gap1 = w == windows.last() ? 0.0 : gap;
                Window *win = w->data();
                CWindow *window = win->ptr();
                auto border = window->getRealBorderSize();
                auto c0 = window->m_vPosition.y - border;
                auto c1 = window->m_vPosition.y - border - gap0 + win->get_geom_h();
                if (c0 < geom.y + geom.h && c0 >= geom.y ||
                    c1 > geom.y && c1 <= geom.y + geom.h ||
                    //should never happen as windows are never taller than the screen
                    c0 < geom.y && c1 >= geom.y + geom.h) {
                    from = w;
                    break;
                }
            }
            for (auto w = windows.last(); w != nullptr; w = w->prev()) {
                auto gap0 = w == windows.first() ? 0.0 : gap;
                auto gap1 = w == windows.last() ? 0.0 : gap;
                Window *win = w->data();
                CWindow *window = win->ptr();
                auto border = window->getRealBorderSize();
                auto c0 = window->m_vPosition.y - border;
                auto c1 = window->m_vPosition.y - border - gap0 + win->get_geom_h();
                if (c0 < geom.y + geom.h && c0 >= geom.y ||
                    c1 > geom.y && c1 <= geom.y + geom.h ||
                    //should never happen as columns are never wider than the screen
                    c0 < geom.y && c1 >= geom.y + geom.h) {
                    to = w;
                    break;
                }
            }
            break;
        case FitSize::All:
            from = windows.first();
            to = windows.last();
            break;
        case FitSize::ToEnd:
            from = active;
            to = windows.last();
            break;
        case FitSize::ToBeg:
            from = windows.first();
            to = active;
            break;
        default:
            return;
        }

        // Now align from top of the screen (geom.y), split height of
        // screen (geom.h) among from->to, and readapt the rest
        if (from != nullptr && to != nullptr) {
            auto c = from;
            double total = 0.0;
            for (auto c = from; c != to->next(); c = c->next()) {
                total += c->data()->get_geom_h();
            }
            for (auto c = from; c != to->next(); c = c->next()) {
                Window *win = c->data();
                win->set_height_free();
                win->set_geom_h(win->get_geom_h() / total * geom.h);
            }
            auto gap0 = from == windows.first() ? 0.0 : gap;
            from->data()->ptr()->m_vPosition.y = geom.y + gap0 + from->data()->ptr()->getRealBorderSize();

            adjust_windows(from, gap_x, gap);
        }
    }
    void cycle_size_active_window(int step, const Vector2D &gap_x, double gap) {
        reorder = Reorder::Auto;
        WindowHeight height = active->data()->get_height();
        if (height == WindowHeight::Free) {
            // When cycle-resizing from Free mode, always move back to One
            height = WindowHeight::One;
        } else {
            int number = static_cast<int>(WindowHeight::Number);
            height =static_cast<WindowHeight>(
                    (number + static_cast<int>(height) + step) % number);
        }
        active->data()->update_height(height, geom.h);
        recalculate_col_geometry(gap_x, gap);
    }
private:
    // Adjust all the windows in the column using 'window' as anchor
    void adjust_windows(ListNode<Window *> *win, const Vector2D &gap_x, double gap) {
        // 2. adjust positions of windows above
        for (auto w = win->prev(), p = win; w != nullptr; p = w, w = w->prev()) {
            CWindow *ww = w->data()->ptr();
            CWindow *pw = p->data()->ptr();
            auto wgap0 = w == windows.first() ? 0.0 : gap;
            auto wborder = ww->getRealBorderSize();
            auto pborder = pw->getRealBorderSize();
            ww->m_vPosition = Vector2D(geom.x + wborder + gap_x.x, pw->m_vPosition.y - gap - pborder - w->data()->get_geom_h() + wborder + wgap0);
        }
        // 3. adjust positions of windows below
        for (auto w = win->next(), p = win; w != nullptr; p = w, w = w->next()) {
            CWindow *ww = w->data()->ptr();
            CWindow *pw = p->data()->ptr();
            auto pgap0 = p == windows.first() ? 0.0 : gap;
            auto wborder = ww->getRealBorderSize();
            auto pborder = pw->getRealBorderSize();
            ww->m_vPosition = Vector2D(geom.x + wborder + gap_x.x, pw->m_vPosition.y - pborder - pgap0 + p->data()->get_geom_h() + wborder + gap);
        }
        for (auto w = windows.first(); w != nullptr; w = w->next()) {
            CWindow *win = w->data()->ptr();
            auto gap0 = w == windows.first() ? 0.0 : gap;
            auto gap1 = w == windows.last() ? 0.0 : gap;
            auto border = win->getRealBorderSize();
            auto wh = w->data()->get_geom_h();
            //win->m_vSize = Vector2D(geom.w - 2.0 * border - gap_x.x - gap_x.y, wh - 2.0 * border - gap0 - gap1);
            win->m_vSize = Vector2D(std::max(geom.w - 2.0 * border - gap_x.x - gap_x.y, 1.0), std::max(wh - 2.0 * border - gap0 - gap1, 1.0));
            win->m_vRealPosition = win->m_vPosition;
            win->m_vRealSize = win->m_vSize;
        }
    }
public:
    void resize_active_window(double maxw, const Vector2D &gap_x, double gap, const Vector2D &delta) {
        // First, check if resize is possible or it would leave any window
        // with an invalid size.

        // Width check
        auto border = active->data()->ptr()->getRealBorderSize();
        auto rwidth = geom.w + delta.x - 2.0 * border - gap_x.x - gap_x.y;
        // Now we check for a size smaller than the maximum possible gap, so
        // we never get in trouble when a window gets expelled from a column
        // with gaps_out, gaps_in, to a column with gaps_in on both sides.
        auto mwidth = geom.w + delta.x - 2.0 * (border + std::max(std::max(gap_x.x, gap_x.y), gap));
        if (mwidth <= 0.0 || rwidth >= maxw)
            return;

        if (std::abs(static_cast<int>(delta.y)) > 0) {
            for (auto win = windows.first(); win != nullptr; win = win->next()) {
                auto gap0 = win == windows.first() ? 0.0 : gap;
                auto gap1 = win == windows.last() ? 0.0 : gap;
                auto wh = win->data()->get_geom_h() - gap0 - gap1 - 2.0 * border;
                if (win == active)
                    wh += delta.y;
                if (wh <= 0.0 || wh + 2.0 * win->data()->ptr()->getRealBorderSize() + gap0 + gap1 > geom.h)
                    // geom.h already includes gaps_out
                    return;
            }
        }
        reorder = Reorder::Auto;
        // Now, resize.
        width = ColumnWidth::Free;

        geom.w += delta.x;
        if (std::abs(static_cast<int>(delta.y)) > 0) {
            for (auto win = windows.first(); win != nullptr; win = win->next()) {
                Window *window = win->data();
                if (win == active)
                    window->set_geom_h(window->get_geom_h() + delta.y);
            }
        }
    }

private:
    struct Memory {
        Box geom;
    };
    ColumnWidth width;
    WindowHeight height;
    Reorder reorder;
    bool initialized;
    Box geom;        // bbox of column
    bool maxdim;     // maximized?
    Memory mem;      // memory of the column's box while in maximized/overview mode
    Box full;        // full screen geometry
    ListNode<Window *> *active;
    List<Window *> windows;
};

class Row {
public:
    Row(CWindow *window)
        : workspace(window->workspaceID()), mode(Mode::Row), reorder(Reorder::Auto),
        overview(false), active(nullptr) {
        update_sizes(g_pCompositor->getMonitorFromID(window->m_iMonitorID));
    }
    ~Row() {
        for (auto col = columns.first(); col != nullptr; col = col->next()) {
            delete col->data();
        }
        columns.clear();
    }
    int get_workspace() const { return workspace; }
    bool has_window(CWindow *window) const {
        for (auto col = columns.first(); col != nullptr; col = col->next()) {
            if (col->data()->has_window(window))
                return true;
        }
        return false;
    }
    CWindow *get_active_window() const {
        return active->data()->get_active_window();
    }
    bool is_active(CWindow *window) const {
        return get_active_window() == window;
    }
    void add_active_window(CWindow *window) {
        if (mode == Mode::Column) {
            active->data()->add_active_window(window, max.h);
            active->data()->recalculate_col_geometry(calculate_gap_x(active), gap);
            return;
        }
        active = columns.emplace_after(active, new Column(window, max.w, max.h));
        reorder = Reorder::Auto;
        recalculate_row_geometry();
    }

    // Remove a window and re-adapt rows and columns, returning
    // true if successful, or false if this is the last row
    // so the layout can remove it.
    bool remove_window(CWindow *window) {
        reorder = Reorder::Auto;
        for (auto c = columns.first(); c != nullptr; c = c->next()) {
            Column *col = c->data();
            if (col->has_window(window)) {
                col->remove_window(window);
                if (col->size() == 0) {
                    if (c == active) {
                        // make NEXT one active before deleting (like PaperWM)
                        // If active was the only one left, doesn't matter
                        // whether it points to end() or not, the row will
                        // be deleted by the parent.
                        active = active != columns.last() ? active->next() : active->prev();
                    }
                    delete col;
                    columns.erase(c);
                    if (columns.empty()) {
                        return false;
                    } else {
                        recalculate_row_geometry();
                        return true;
                    }
                } else {
                    c->data()->recalculate_col_geometry(calculate_gap_x(c), gap);
                    return true;
                }
            }
        }
        return true;
    }
    void focus_window(CWindow *window) {
        for (auto c = columns.first(); c != nullptr; c = c->next()) {
            if (c->data()->has_window(window)) {
                c->data()->focus_window(window);
                active = c;
                recalculate_row_geometry();
                return;
            }
        }
    }
    bool move_focus(Direction dir, bool focus_wrap) {
        reorder = Reorder::Auto;
        switch (dir) {
        case Direction::Left:
            if (!move_focus_left(focus_wrap))
                return false;
            break;
        case Direction::Right:
            if (!move_focus_right(focus_wrap))
                return false;
            break;
        case Direction::Up:
            if (!active->data()->move_focus_up(focus_wrap))
                return false;
            break;
        case Direction::Down:
            if (!active->data()->move_focus_down(focus_wrap))
                return false;
            break;
        case Direction::Begin:
            move_focus_begin();
            break;
        case Direction::End:
            move_focus_end();
            break;
        default:
            return true;
        }
        recalculate_row_geometry();
        return true;
    }

private:
    bool move_focus_left(bool focus_wrap) {
        if (active == columns.first()) {
            CMonitor *monitor = g_pCompositor->getMonitorInDirection('l');
            if (monitor == nullptr) {
                if (focus_wrap)
                    active = columns.last();
                return true;
            }

            g_pKeybindManager->m_mDispatchers["movefocus"]("l");
            return false;
        }
        active = active->prev();
        return true;
    }
    bool move_focus_right(bool focus_wrap) {
        if (active == columns.last()) {
            CMonitor *monitor = g_pCompositor->getMonitorInDirection('r');
            if (monitor == nullptr) {
                if (focus_wrap)
                    active = columns.first();
                return true;
            }

            g_pKeybindManager->m_mDispatchers["movefocus"]("r");
            return false;
        }
        active = active->next();
        return true;
    }
    void move_focus_begin() {
        active = columns.first();
    }
    void move_focus_end() {
        active = columns.last();
    }

    // Calculate lateral gaps for a column
    Vector2D calculate_gap_x(const ListNode<Column *> *column) const {
        // First and last columns need a different gap
        auto gap0 = column == columns.first() ? 0.0 : gap;
        auto gap1 = column == columns.last() ? 0.0 : gap;
        return Vector2D(gap0, gap1);
    }

public:
    void resize_active_column(int step) {
        if (active->data()->maximized())
            return;

        if (mode == Mode::Column) {
            active->data()->cycle_size_active_window(step, calculate_gap_x(active), gap);
            return;
        }

        ColumnWidth width = active->data()->get_width();
        if (width == ColumnWidth::Free) {
            // When cycle-resizing from Free mode, always move back to OneHalf
            width = ColumnWidth::OneHalf;
        } else {
            int number = static_cast<int>(ColumnWidth::Number);
            width =static_cast<ColumnWidth>(
                    (number + static_cast<int>(width) + step) % number);
        }
        active->data()->update_width(width, max.w, max.h);
        reorder = Reorder::Auto;
        recalculate_row_geometry();
    }
    void resize_active_window(const Vector2D &delta) {
        // If the active window in the active column is fullscreen, ignore.
        if (active->data()->maximized() ||
            active->data()->fullscreen())
            return;

        active->data()->resize_active_window(max.w, calculate_gap_x(active), gap, delta);
        recalculate_row_geometry();
    }
    void set_mode(Mode m) {
        mode = m;
    }
    void align_column(Direction dir) {
        if (active->data()->maximized() ||
            active->data()->fullscreen())
            return;

        switch (dir) {
        case Direction::Left:
            active->data()->set_geom_pos(max.x, max.y);
            break;
        case Direction::Right:
            active->data()->set_geom_pos(max.x + max.w - active->data()->get_geom_w(), max.y);
            break;
        case Direction::Center:
            if (mode == Mode::Column) {
                active->data()->align_window(Direction::Center, gap);
                active->data()->recalculate_col_geometry(calculate_gap_x(active), gap);
            } else {
                center_active_column();
            }
            break;
        case Direction::Up:
        case Direction::Down:
            active->data()->align_window(dir, gap);
            active->data()->recalculate_col_geometry(calculate_gap_x(active), gap);
            break;
        default:
            return;
        }
        reorder = Reorder::Lazy;
        recalculate_row_geometry();
    }
private:
    void center_active_column() {
        Column *column = active->data();
        if (column->maximized())
            return;

        switch (column->get_width()) {
        case ColumnWidth::OneThird:
            column->set_geom_pos(max.x + max.w / 3.0, max.y);
            break;
        case ColumnWidth::OneHalf:
            column->set_geom_pos(max.x + max.w / 4.0, max.y);
            break;
        case ColumnWidth::TwoThirds:
            column->set_geom_pos(max.x + max.w / 6.0, max.y);
            break;
        case ColumnWidth::Free:
            column->set_geom_pos(0.5 * (max.w - column->get_geom_w()), max.y);
            break;
        default:
            break;
        }
    }
public:
    void move_active_column(Direction dir) {
        switch (dir) {
        case Direction::Right:
            if (active != columns.last()) {
                auto next = active->next();
                columns.swap(active, next);
            }
            break;
        case Direction::Left:
            if (active != columns.first()) {
                auto prev = active->prev();
                columns.swap(active, prev);
            }
            break;
        case Direction::Up:
            active->data()->move_active_up();
            break;
        case Direction::Down:
            active->data()->move_active_down();
            break;
        case Direction::Begin: {
            if (active == columns.first())
                break;
            columns.move_before(columns.first(), active);
            break;
        }
        case Direction::End: {
            if (active == columns.last())
                break;
            columns.move_after(columns.last(), active);
            break;
        }
        case Direction::Center:
            return;
        }

        reorder = Reorder::Auto;
        recalculate_row_geometry();
    }
    void admit_window_left() {
        if (active->data()->maximized() ||
            active->data()->fullscreen())
            return;
        if (active == columns.first())
            return;

        auto w = active->data()->expel_active(gap);
        auto prev = active->prev();
        if (active->data()->size() == 0) {
            columns.erase(active);
        }
        active = prev;
        active->data()->admit_window(w);

        reorder = Reorder::Auto;
        recalculate_row_geometry();
    }
    void expel_window_right() {
        if (active->data()->maximized() ||
            active->data()->fullscreen())
            return;
        if (active->data()->size() == 1)
            // nothing to expel
            return;

        auto w = active->data()->expel_active(gap);
        ColumnWidth width = active->data()->get_width();
        // This code inherits the width of the original column. There is a
        // problem with that when the mode is "Free". The new column may have
        // more reserved space for gaps, and the new window in that column
        // end up having negative size --> crash.
        // There are two options:
        // 1. We don't let column resizing make a column smaller than gap
        // 2. We compromise and inherit the ColumnWidth attribute unless it is
        // "Free". In that case, we force OneHalf (the default).
#if 1
        double maxw = width == ColumnWidth::Free ? active->data()->get_geom_w() : max.w;
#else
        double maxw = max.w;
        if (width == ColumnWidth::Free)
            width = ColumnWidth::OneHalf;
#endif
        active = columns.emplace_after(active, new Column(w, width, maxw, max.h));
        // Initialize the position so it is located after its previous column
        // This helps the heuristic in recalculate_row_geometry()
        active->data()->set_geom_pos(active->prev()->data()->get_geom_x() + active->prev()->data()->get_geom_w(), max.y);

        reorder = Reorder::Auto;
        recalculate_row_geometry();
    }
    Vector2D predict_window_size() const {
        return Vector2D(0.5 * max.w, max.h);
    }
    void update_sizes(CMonitor *monitor) {
        // for gaps outer
        static auto PGAPSINDATA = CConfigValue<Hyprlang::CUSTOMTYPE>("general:gaps_in");
        static auto PGAPSOUTDATA = CConfigValue<Hyprlang::CUSTOMTYPE>("general:gaps_out");
        auto *const PGAPSIN = (CCssGapData *)(PGAPSINDATA.ptr())->getData();
        auto *const PGAPSOUT = (CCssGapData *)(PGAPSOUTDATA.ptr())->getData();
        // For now, support only constant CCssGapData
        auto gaps_in = PGAPSIN->top;
        auto gaps_out = PGAPSOUT->top;

        const auto SIZE = monitor->vecSize;
        const auto POS = monitor->vecPosition;
        const auto TOPLEFT = monitor->vecReservedTopLeft;
        const auto BOTTOMRIGHT = monitor->vecReservedBottomRight;

        full = Box(POS, SIZE);
        max = Box(POS.x + TOPLEFT.x + gaps_out,
                POS.y + TOPLEFT.y + gaps_out,
                SIZE.x - TOPLEFT.x - BOTTOMRIGHT.x - 2 * gaps_out,
                SIZE.y - TOPLEFT.y - BOTTOMRIGHT.y - 2 * gaps_out);
        gap = gaps_in;
    }
    void set_fullscreen_active_window() {
        active->data()->set_fullscreen(full);
        // Parameters here don't matter
        active->data()->recalculate_col_geometry(calculate_gap_x(active), gap);
    }
    void toggle_fullscreen_active_window() {
        Column *column = active->data();
        const auto PWORKSPACE = g_pCompositor->getWorkspaceByID(
            column->get_active_window()->workspaceID());

        auto fullscreen = active->data()->toggle_fullscreen(full);
        PWORKSPACE->m_bHasFullscreenWindow = fullscreen;

        if (fullscreen) {
            PWORKSPACE->m_efFullscreenMode = eFullscreenMode::FULLSCREEN_FULL;
            column->recalculate_col_geometry(calculate_gap_x(active), gap);
        } else {
            recalculate_row_geometry();
        }
    }
    void toggle_maximize_active_column() {
        Column *column = active->data();
        column->toggle_maximized(max.w, max.h);
        reorder = Reorder::Auto;
        recalculate_row_geometry();
    }

    void fit_size(FitSize fitsize) {
        if (mode == Mode::Column) {
            active->data()->fit_size(fitsize, calculate_gap_x(active), gap);
            return;
        }
        ListNode<Column *> *from, *to;
        switch (fitsize) {
        case FitSize::Active:
            from = to = active;
            break;
        case FitSize::Visible:
            for (auto c = columns.first(); c != nullptr; c = c->next()) {
                Column *col = c->data();
                auto c0 = col->get_geom_x();
                auto c1 = col->get_geom_x() + col->get_geom_w();
                if (c0 < max.x + max.w && c0 >= max.x ||
                    c1 > max.x && c1 <= max.x + max.w ||
                    //should never happen as columns are never wider than the screen
                    c0 < max.x && c1 >= max.x + max.w) {
                    from = c;
                    break;
                }
            }
            for (auto c = columns.last(); c != nullptr; c = c->prev()) {
                Column *col = c->data();
                auto c0 = col->get_geom_x();
                auto c1 = col->get_geom_x() + col->get_geom_w();
                if (c0 < max.x + max.w && c0 >= max.x ||
                    c1 > max.x && c1 <= max.x + max.w ||
                    //should never happen as columns are never wider than the screen
                    c0 < max.x && c1 >= max.x + max.w) {
                    to = c;
                    break;
                }
            }
            break;
        case FitSize::All:
            from = columns.first();
            to = columns.last();
            break;
        case FitSize::ToEnd:
            from = active;
            to = columns.last();
            break;
        case FitSize::ToBeg:
            from = columns.first();
            to = active;
            break;
        default:
            return;
        }

        // Now align from to left edge of the screen (max.x), split width of
        // screen (max.w) among from->to, and readapt the rest
        if (from != nullptr && to != nullptr) {
            auto c = from;
            double total = 0.0;
            for (auto c = from; c != to->next(); c = c->next()) {
                total += c->data()->get_geom_w();
            }
            for (auto c = from; c != to->next(); c = c->next()) {
                Column *col = c->data();
                col->set_width_free();
                col->set_geom_w(col->get_geom_w() / total * max.w);
            }
            from->data()->set_geom_pos(max.x, max.y);

            adjust_columns(from);
        }
    }
    void toggle_overview() {
        overview = !overview;
        if (overview) {
            // Find the bounding box
            Vector2D bmin(max.x + max.w, max.y + max.h);
            Vector2D bmax(max.x, max.y);
            for (auto c = columns.first(); c != nullptr; c = c->next()) {
                auto cx0 = c->data()->get_geom_x();
                auto cx1 = cx0 + c->data()->get_geom_w();
                Vector2D cheight = c->data()->get_height();
                if (cx0 < bmin.x)
                    bmin.x = cx0;
                if (cx1 > bmax.x)
                    bmax.x = cx1;
                if (cheight.x < bmin.y)
                    bmin.y = cheight.x;
                if (cheight.y > bmax.y)
                    bmax.y = cheight.y;
            }
            double w = bmax.x - bmin.x;
            double h = bmax.y - bmin.y;
            double scale = std::min(max.w / w, max.h / h);
            for (auto c = columns.first(); c != nullptr; c = c->next()) {
                Column *col = c->data();
                col->push_geom();
                Vector2D cheight = col->get_height();
                Vector2D offset(0.5 * (max.w - w * scale), 0.5 * (max.h - h * scale));
                col->set_geom_pos(offset.x + max.x + (col->get_geom_x() - bmin.x) * scale, offset.y + max.y + (cheight.x - bmin.y) * scale);
                col->set_geom_w(col->get_geom_w() * scale);
                Vector2D start(offset.x + max.x, offset.y + max.y);
                col->scale(bmin, start, scale, gap);
            }
            adjust_columns(columns.first());
        } else {
            for (auto c = columns.first(); c != nullptr; c = c->next()) {
                Column *col = c->data();
                col->pop_geom();
            }
            // Try to maintain the positions except if the active is not visible,
            // in that case, make it visible.
            Column *acolumn = active->data();
            if (acolumn->get_geom_x() < max.x) {
                acolumn->set_geom_pos(max.x, max.y);
            } else if (acolumn->get_geom_x() + acolumn->get_geom_w() > max.x + max.w) {
                acolumn->set_geom_pos(max.x + max.w - acolumn->get_geom_w(), max.y);
            }
            adjust_columns(active);
        }
    }

    void recalculate_row_geometry() {
        if (active == nullptr)
            return;

        if (active->data()->fullscreen()) {
            active->data()->recalculate_col_geometry(calculate_gap_x(active), gap);
            return;
        }
#ifdef COLORS_IPC
        // Change border color
    	static auto *const FREECOLUMN = (CGradientValueData *) HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:col.freecolumn_border")->data.get();
        static auto *const ACTIVECOL = (CGradientValueData *)g_pConfigManager->getConfigValuePtr("general:col.active_border")->data.get();
        if (active->data()->get_width() == ColumnWidth::Free) {
            active->data()->get_active_window()->m_cRealBorderColor = *FREECOLUMN;
        } else {
            active->data()->get_active_window()->m_cRealBorderColor = *ACTIVECOL;
        }
        g_pEventManager->postEvent(SHyprIPCEvent{"scroller", active->data()->get_width_name() + "," + active->data()->get_height_name()});
#endif
        auto a_w = active->data()->get_geom_w();
        double a_x;
        if (active->data()->get_init()) {
            a_x = active->data()->get_geom_x();
        } else {
            // If the column hasn't been initialized yet (newly created window),
            // we know it will be located on the right of active->prev()
            if (active->prev()) {
                // there is a previous one, locate it on its right
                Column *prev = active->prev()->data();
                a_x = prev->get_geom_x() + prev->get_geom_w();
            } else {
                // first window, locate it at the center
                a_x = max.x + 0.5 * (max.w - a_w);
            }
            // mark column as initialized
            active->data()->set_init();
        }
        if (a_x < max.x) {
            // active starts outside on the left
            // set it on the left edge
            active->data()->set_geom_pos(max.x, max.y);
        } else if (std::round(a_x + a_w) > max.x + max.w) {
            // active overflows to the right, move to end of viewport
            active->data()->set_geom_pos(max.x + max.w - a_w, max.y);
        } else {
            // active is inside the viewport
            if (reorder == Reorder::Auto) {
                // The active column should always be completely in the viewport.
                // If any of the windows next to it on its right or left are
                // in the viewport, keep the current position.
                bool keep_current = false;
                if (active->prev() != nullptr) {
                    Column *prev = active->prev()->data();
                    if (prev->get_geom_x() >= max.x && prev->get_geom_x() + prev->get_geom_w() <= max.x + max.w) {
                        keep_current = true;
                    }
                }
                if (!keep_current && active->next() != nullptr) {
                    Column *next = active->next()->data();
                    if (next->get_geom_x() >= max.x && next->get_geom_x() + next->get_geom_w() <= max.x + max.w) {
                        keep_current = true;
                    }
                }
                if (!keep_current) {
                    // If not:
                    // We try to fit the column next to it on the right if it fits
                    // completely, otherwise the one on the left. If none of them fit,
                    // we leave it as it is.
                    if (active->next() != nullptr) {
                        if (a_w + active->next()->data()->get_geom_w() <= max.w) {
                            // set next at the right edge of the viewport
                            active->data()->set_geom_pos(max.x + max.w - a_w - active->next()->data()->get_geom_w(), max.y);
                        } else if (active->prev() != nullptr) {
                            if (active->prev()->data()->get_geom_w() + a_w <= max.w) {
                                // set previous at the left edge of the viewport
                                active->data()->set_geom_pos(max.x + active->prev()->data()->get_geom_w(), max.y);
                            } else {
                                // none of them fit, leave active as it is
                                active->data()->set_geom_pos(a_x, max.y);
                            }
                        } else {
                            // nothing on the left, move active to left edge of viewport
                            active->data()->set_geom_pos(max.x, max.y);
                        }
                    } else if (active->prev() != nullptr) {
                        if (active->prev()->data()->get_geom_w() + a_w <= max.w) {
                            // set previous at the left edge of the viewport
                            active->data()->set_geom_pos(max.x + active->prev()->data()->get_geom_w(), max.y);
                        } else {
                            // it doesn't fit and nothing on the right, move active to right edge of viewport
                            active->data()->set_geom_pos(max.x + max.w - a_w, max.y);
                        }
                    } else {
                        // nothing on the right or left, the window is in a correct position
                        active->data()->set_geom_pos(a_x, max.y);
                    }
                } else {
                    // the window is in a correct position
                    active->data()->set_geom_pos(a_x, max.y);
                }
            } else {  // lazy
                // Try to avoid moving the active column unless it is out of the screen.
                // the window is in a correct position
                active->data()->set_geom_pos(a_x, max.y);
            }
        }

        adjust_columns(active);
    }

private:
    // Adjust all the columns in the row using 'column' as anchor
    void adjust_columns(ListNode<Column *> *column) {
        // Adjust the positions of the columns to the left
        for (auto col = column->prev(), prev = column; col != nullptr; prev = col, col = col->prev()) {
            col->data()->set_geom_pos(prev->data()->get_geom_x() - col->data()->get_geom_w(), max.y);
        }
        // Adjust the positions of the columns to the right
        for (auto col = column->next(), prev = column; col != nullptr; prev = col, col = col->next()) {
            col->data()->set_geom_pos(prev->data()->get_geom_x() + prev->data()->get_geom_w(), max.y);
        }

        // Apply column geometry
        for (auto col = columns.first(); col != nullptr; col = col->next()) {
            // First and last columns need a different gap
            auto gap0 = col == columns.first() ? 0.0 : gap;
            auto gap1 = col == columns.last() ? 0.0 : gap;
            col->data()->recalculate_col_geometry(Vector2D(gap0, gap1), gap);
        }
    }

    int workspace;
    Box full;
    Box max;
    bool overview;
    int gap;
    Reorder reorder;
    Mode mode;
    ListNode<Column *> *active;
    List<Column *> columns;
};


Row *ScrollerLayout::getRowForWorkspace(int workspace) {
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        if (row->data()->get_workspace() == workspace)
            return row->data();
    }
    return nullptr;
}

Row *ScrollerLayout::getRowForWindow(CWindow *window) {
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        if (row->data()->has_window(window))
            return row->data();
    }
    return nullptr;
}

/*
    Called when a window is created (mapped)
    The layout HAS TO set the goal pos and size (anim mgr will use it)
    If !animationinprogress, then the anim mgr will not apply an anim.
*/
void ScrollerLayout::onWindowCreatedTiling(CWindow *window, eDirection)
{
    auto s = getRowForWorkspace(window->workspaceID());
    if (s == nullptr) {
        s = new Row(window);
        rows.push_back(s);
    }
    s->add_active_window(window);
}

/*
    Called when a window is removed (unmapped)
*/
void ScrollerLayout::onWindowRemovedTiling(CWindow *window)
{
    marks.remove(window);

    auto s = getRowForWindow(window);
    if (s == nullptr) {
        return;
    }
    if (!s->remove_window(window)) {
        // It was the last one, remove the row
        for (auto row = rows.first(); row != nullptr; row = row->next()) {
            if (row->data() == s) {
                rows.erase(row);
                delete row->data();
                return;
            }
        }
    }
}

/*
    Internal: called when window focus changes
*/
void ScrollerLayout::onWindowFocusChange(CWindow *window)
{
    if (window == nullptr) { // no window has focus
        return;
    }

    auto s = getRowForWindow(window);
    if (s == nullptr) {
        return;
    }
    s->focus_window(window);
}

/*
    Return tiled status
*/
bool ScrollerLayout::isWindowTiled(CWindow *window)
{
    return getRowForWindow(window) != nullptr;
}

/*
    Called when the monitor requires a layout recalculation
    this usually means reserved area changes
*/
void ScrollerLayout::recalculateMonitor(const int &monitor_id)
{
    auto PMONITOR = g_pCompositor->getMonitorFromID(monitor_id);
    if (!PMONITOR)
        return;

    g_pHyprRenderer->damageMonitor(PMONITOR);

    auto PWORKSPACE = PMONITOR->activeWorkspace;
    if (!PWORKSPACE)
        return;

    auto s = getRowForWorkspace(PWORKSPACE->m_iID);
    if (s == nullptr)
        return;

    s->update_sizes(PMONITOR);
    if (PWORKSPACE->m_bHasFullscreenWindow && PWORKSPACE->m_efFullscreenMode == FULLSCREEN_FULL) {
        s->set_fullscreen_active_window();
    } else {
        s->recalculate_row_geometry();
    }
    if (PMONITOR->activeSpecialWorkspaceID()) {
        auto sw = getRowForWorkspace(PMONITOR->activeSpecialWorkspaceID());
        if (sw == nullptr) {
            return;
        }
        sw->update_sizes(PMONITOR);
        sw->recalculate_row_geometry();
    }
}

/*
    Called when the compositor requests a window
    to be recalculated, e.g. when pseudo is toggled.
*/
void ScrollerLayout::recalculateWindow(CWindow *window)
{
    auto s = getRowForWindow(window);
    if (s == nullptr)
        return;

    s->recalculate_row_geometry();
}

/*
    Called when a user requests a resize of the current window by a vec
    Vector2D holds pixel values
    Optional pWindow for a specific window
*/
void ScrollerLayout::resizeActiveWindow(const Vector2D &delta,
                                        eRectCorner corner, CWindow *window)
{
    const auto PWINDOW = window ? window : g_pCompositor->m_pLastWindow;
    auto s = getRowForWindow(PWINDOW);
    if (s == nullptr)
        return;

    s->resize_active_window(delta);
}

/*
   Called when a window / the user requests to toggle the fullscreen state of a
   window. The layout sets all the fullscreen flags. It can either accept or
   ignore.
*/
void ScrollerLayout::fullscreenRequestForWindow(CWindow *window,
                                                eFullscreenMode fullscreenmode,
                                                bool on)
{
    auto s = getRowForWindow(window);
    if (s == nullptr) {
        return;
    } // assuming window is active for now

    const auto PWORKSPACE = g_pCompositor->getWorkspaceByID(window->workspaceID());
    switch (fullscreenmode) {
        case eFullscreenMode::FULLSCREEN_FULL:
            if (on == window->m_bIsFullscreen)
                return;

            // if the window wants to be fullscreen but there already is one, ignore the request.
            if (PWORKSPACE->m_bHasFullscreenWindow && on)
                return;

            s->toggle_fullscreen_active_window();
            break;
        case eFullscreenMode::FULLSCREEN_MAXIMIZED:
            s->toggle_maximize_active_column();
            break;
        default:
            return;
    }
}

/*
    Called when a dispatcher requests a custom message
    The layout is free to ignore.
    std::any is the reply. Can be empty.
*/
std::any ScrollerLayout::layoutMessage(SLayoutMessageHeader header, std::string content)
{
    return "";
}

/*
    Required to be handled, but may return just SWindowRenderLayoutHints()
    Called when the renderer requests any special draw flags for
    a specific window, e.g. border color for groups.
*/
SWindowRenderLayoutHints ScrollerLayout::requestRenderHints(CWindow *)
{
    return {};
}

/*
    Called when the user requests two windows to be swapped places.
    The layout is free to ignore.
*/
void ScrollerLayout::switchWindows(CWindow *, CWindow *)
{
}

/*
    Called when the user requests a window move in a direction.
    The layout is free to ignore.
*/
void ScrollerLayout::moveWindowTo(CWindow *window, const std::string &direction)
{
    auto s = getRowForWindow(window);
    if (s == nullptr) {
        return;
    } else if (!(s->is_active(window))) {
        // cannot move non active window?
        return;
    }

    switch (direction.at(0)) {
        case 'l': s->move_active_column(Direction::Left); break;
        case 'r': s->move_active_column(Direction::Right); break;
        case 'u': s->move_active_column(Direction::Up); break;
        case 'd': s->move_active_column(Direction::Down); break;
        default: break;
    }
}

/*
    Called when the user requests to change the splitratio by or to X
    on a window
*/
void ScrollerLayout::alterSplitRatio(CWindow *, float, bool)
{
}

/*
    Called when something wants the current layout's name
*/
std::string ScrollerLayout::getLayoutName()
{
    return "scroller";
}

/*
    Called for getting the next candidate for a focus
*/
CWindow* ScrollerLayout::getNextWindowCandidate(CWindow *old_window)
{
    // This is called when a windows in unmapped. This means the window
    // has also been removed from the layout. In that case, returning the
    // new active window is the correct thing.
    int workspace_id = g_pCompositor->m_pLastMonitor->activeWorkspaceID();
    auto s = getRowForWorkspace(workspace_id);
    if (s == nullptr)
        return nullptr;
    else
        return s->get_active_window();
}

/*
    Called for replacing any data a layout has for a new window
*/
void ScrollerLayout::replaceWindowDataWith(CWindow *from, CWindow *to)
{
}

void ScrollerLayout::onEnable() {
    marks.reset();
    for (auto& window : g_pCompositor->m_vWindows) {
        if (window->m_bIsFloating || !window->m_bIsMapped || window->isHidden())
            continue;

        onWindowCreatedTiling(window.get());
        recalculateMonitor(window.get()->m_iMonitorID);
    }
}

void ScrollerLayout::onDisable() {
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        delete row->data();
    }
    rows.clear();
    marks.reset();
}

/*
    Called to predict the size of a newly opened window to send it a configure.
    Return 0,0 if unpredictable
*/
Vector2D ScrollerLayout::predictSizeForNewWindowTiled() {
    if (!g_pCompositor->m_pLastMonitor)
        return {};

    int workspace_id = g_pCompositor->m_pLastMonitor->activeWorkspaceID();
    auto s = getRowForWorkspace(workspace_id);
    if (s == nullptr) {
        Vector2D size =g_pCompositor->m_pLastMonitor->vecSize;
        size.x *= 0.5;
        return size;
    }

    return s->predict_window_size();
}

void ScrollerLayout::cycle_window_size(int workspace, int step)
{
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->resize_active_column(step);
}

static void switch_to_window(CWindow *window)
{
    if (window == g_pCompositor->m_pLastWindow)
        return;

    g_pInputManager->unconstrainMouse();
    g_pCompositor->focusWindow(window);
    g_pCompositor->warpCursorTo(window->middle());

    g_pInputManager->m_pForcedFocus = window;
    g_pInputManager->simulateMouseMovement();
    g_pInputManager->m_pForcedFocus = nullptr;
}

void ScrollerLayout::move_focus(int workspace, Direction direction)
{
    static auto* const *focus_wrap = (Hyprlang::INT* const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:focus_wrap")->getDataStaticPtr();
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        // if workspace is empty, use the deault movefocus, which now
        // is "move to another monitor" (pass the direction)
        switch (direction) {
            case Direction::Left:
                g_pKeybindManager->m_mDispatchers["movefocus"]("l");
                break;
            case Direction::Right:
                g_pKeybindManager->m_mDispatchers["movefocus"]("r");
                break;
            case Direction::Up:
                g_pKeybindManager->m_mDispatchers["movefocus"]("u");
                break;
            case Direction::Down:
                g_pKeybindManager->m_mDispatchers["movefocus"]("d");
                break;
            default:
                break;
        }
        return;
    }

    if (!s->move_focus(direction, **focus_wrap == 0 ? false : true)) {
        // changed monitor
        s = getRowForWorkspace(g_pCompositor->m_pLastMonitor->activeWorkspaceID());
        if (s == nullptr) {
            // monitor is empty
            return;
        }
    }
    switch_to_window(s->get_active_window());
}

void ScrollerLayout::move_window(int workspace, Direction direction) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->move_active_column(direction);
    switch_to_window(s->get_active_window());
}

void ScrollerLayout::align_window(int workspace, Direction direction) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->align_column(direction);
}

void ScrollerLayout::admit_window_left(int workspace) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->admit_window_left();
}

void ScrollerLayout::expel_window_right(int workspace) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->expel_window_right();
}

void ScrollerLayout::set_mode(int workspace, Mode mode) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->set_mode(mode);
}

void ScrollerLayout::fit_size(int workspace, FitSize fitsize) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->fit_size(fitsize);
}

void ScrollerLayout::toggle_overview(int workspace) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->toggle_overview();
}

static int get_workspace_id() {
    int workspace_id;
    if (g_pCompositor->m_pLastMonitor->activeSpecialWorkspaceID()) {
        workspace_id = g_pCompositor->m_pLastMonitor->activeSpecialWorkspaceID();
    } else {
        workspace_id = g_pCompositor->m_pLastMonitor->activeWorkspaceID();
    }
    if (workspace_id == WORKSPACE_INVALID)
        return -1;
    if (g_pCompositor->getWorkspaceByID(workspace_id) == nullptr)
        return -1;

    return workspace_id;
}

void ScrollerLayout::marks_add(const std::string &name) {
    CWindow *w = getRowForWorkspace(get_workspace_id())->get_active_window();
    marks.add(w, name);
}

void ScrollerLayout::marks_delete(const std::string &name) {
    marks.del(name);
}

void ScrollerLayout::marks_visit(const std::string &name) {
    CWindow *window = marks.visit(name);
    if (window != nullptr)
        switch_to_window(window);
}

void ScrollerLayout::marks_reset() {
    marks.reset();
}
