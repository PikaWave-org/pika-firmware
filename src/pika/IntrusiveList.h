#pragma once

namespace pika {

template<class T>
class IntrusiveList;

template<class T>
class IntrusiveListItem {
public:
    ~IntrusiveListItem() {
        if (_next) {
            _next->_prev = _prev;
        }
        *_prev = _next;
    }

protected:
    IntrusiveList<T> *_list = nullptr;

    IntrusiveListItem() {}

    explicit IntrusiveListItem(IntrusiveList<T> &list) : _list(&list) {
        _next = list._items;
        _prev = &list._items;
        _list->_items = this;
    }

private:
    friend class IntrusiveList<T>;

    IntrusiveListItem *_next = nullptr;
    IntrusiveListItem **_prev = nullptr;
};

template<class T>
class IntrusiveList {
public:
    IntrusiveList() = default;

    template<class F>
    void for_each(F &&f) {
        IntrusiveListItem<T> *curr = _items;
        while (curr) {
            IntrusiveListItem<T> *next = curr->_next;
            f(static_cast<T &>(*curr));
            curr = next;
        }
    }

    void add(IntrusiveListItem<T> &item) {
        if (item._list != this) {
            item._list = this;
            item._next = _items;
            item._prev = &_items;
            _items = &item;
        }
    }

    void remove(IntrusiveListItem<T> &item) {
        if (item._list == this) {
            if (item._next) {
                item._next->_prev = item._prev;
            }
            *item._prev = item._next;

            item._list = nullptr;
            item._next = nullptr;
            item._prev = nullptr;
        }
    }

private:
    friend class IntrusiveListItem<T>;

    IntrusiveListItem<T> *_items = nullptr;
};

}// namespace pika
