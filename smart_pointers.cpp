#include <iostream>
#include <memory>

template <typename T>
class EnableSharedFromThis;

struct BaseControlBlock {
    BaseControlBlock(int shared = 0, int weak = 0)
        : shared_counter(shared), weak_counter(weak) {}
    BaseControlBlock(const BaseControlBlock& other)
        : shared_counter(other.shared_counter),
          weak_counter(other.weak_counter) {}
    int shared_counter;
    int weak_counter;
    virtual void use_deleter() = 0;
    virtual void call_destructor() {}
    virtual void deallocate() = 0;
    virtual ~BaseControlBlock() = default;
    bool operator==(const BaseControlBlock& other) const {
        return shared_counter == other.shared_counter &&
               weak_counter == other.weak_counter;
    }

    virtual BaseControlBlock& operator=(const BaseControlBlock& other) {
        shared_counter = other.shared_counter;
        weak_counter = other.weak_counter;
        return *this;
    }
};

template <typename T>
class SharedPtr {
    template <typename Tp, typename Alloc, typename... Args>
    friend SharedPtr<Tp> allocateShared(const Alloc& alloc, Args&&... args);

    template <typename Tp, typename... Args>
    friend SharedPtr<Tp> makeShared(Args&&... args);

    template <typename U>
    friend class SharedPtr;

    template <typename U>
    friend class WeakPtr;

  private:
    template <typename Deleter = std::default_delete<T>,
              typename Alloc = std::allocator<T>>
    struct ControlBlockRegular : BaseControlBlock {
        ControlBlockRegular(int shared = 0, int weak = 0, T* p = nullptr,
                            Deleter del = std::default_delete<T>(),
                            Alloc all = std::allocator<T>())
            : BaseControlBlock(shared, weak),
              object(p),
              deleter(del),
              alloc(all) {}

        ControlBlockRegular(const ControlBlockRegular& other)
            : BaseControlBlock(other.shared_counter, other.weak_counter),
              object(other.object),
              deleter(other.deleter),
              alloc(other.alloc) {}

        T* object;
        Deleter deleter;
        Alloc alloc;

        bool operator==(const ControlBlockRegular& other) const {
            return object == other.object && deleter == other.deleter && alloc =
                       other.alloc;
        }

        void use_deleter() override {
            if (object != nullptr) {
                deleter(object);
            }
        }

        void call_destructor() override {}
        void deallocate() override {
            using CB = ControlBlockRegular<Deleter, Alloc>;
            using CBAlloc = typename std::allocator_traits<
                Alloc>::template rebind_alloc<CB>;
            CBAlloc alloc_cb = alloc;
            std::allocator_traits<CBAlloc>::deallocate(alloc_cb, this, 1);
        }
    };

    template <typename Alloc = std::allocator<T>>
    struct ControlBlockMakeShared : BaseControlBlock {
        ControlBlockMakeShared(int shared, int weak, T&& ptr,
                               Alloc all = std::allocator<T>())
            : BaseControlBlock(shared, weak),
              object(std::move(ptr)),
              alloc(std::move(all)) {}

        ControlBlockMakeShared(int shared, int weak, const T& ptr,
                               Alloc all = std::allocator<T>())
            : BaseControlBlock(shared, weak), alloc(all) {
            object = ptr;
        }

        template <typename... Args>
        ControlBlockMakeShared(int shared, int weak, Args&&... args,
                               Alloc all = std::allocator<T>())
            : BaseControlBlock(shared, weak),
              object(T(std::forward<Args>(args)...)),
              alloc(all) {}

        ControlBlockMakeShared(const ControlBlockMakeShared& other)
            : BaseControlBlock(other.shared_counter, other.weak_counter),
              object(other.object),
              alloc(other.alloc) {}

        T object;
        Alloc alloc;

        bool operator==(const ControlBlockMakeShared& other) const {
            return object == other.object && alloc = other.alloc;
        }

        void use_deleter() override {}

        void call_destructor() override {
            std::allocator_traits<Alloc>::destroy(alloc, &object);
        }
        void deallocate() override {
            using CB = ControlBlockMakeShared<Alloc>;
            using CBAlloc = typename std::allocator_traits<
                Alloc>::template rebind_alloc<CB>;
            CBAlloc alloc_cb = alloc;
            std::allocator_traits<CBAlloc>::deallocate(alloc_cb, this, 1);
        }
    };

    T* ptr;
    BaseControlBlock* cb;

    SharedPtr(BaseControlBlock* other_cb) : ptr(nullptr), cb(other_cb) {}

  public:
    SharedPtr() {
        ptr = nullptr;
        cb = nullptr;
    }

    template <typename U>
    SharedPtr(const SharedPtr<U>& other) : ptr(other.ptr), cb(other.cb) {
        ++cb->shared_counter;
        if constexpr (std::is_base_of_v<EnableSharedFromThis<T>, T>) {
            ptr->wptr = *this;
        }
    }

    template <typename U, typename Alloc = std::allocator<U>,
              typename Deleter = std::default_delete<U>>
    SharedPtr(U* pt, Deleter deleter = Deleter(), Alloc alloc = Alloc())
        : ptr(pt) {
        using AllocControlBlock =
            typename std::allocator_traits<Alloc>::template rebind_alloc<
                typename SharedPtr<U>::template ControlBlockRegular<Deleter,
                                                                    Alloc>>;
        using traits = std::allocator_traits<AllocControlBlock>;
        AllocControlBlock alloc_cb = alloc;
        typename SharedPtr<U>::template ControlBlockRegular<Deleter, Alloc>*
            new_cb = traits::allocate(alloc_cb, 1);
        new (new_cb)
            typename SharedPtr<U>::template ControlBlockRegular<Deleter, Alloc>(
                1, 0, ptr, deleter, alloc);
        cb = new_cb;

        if constexpr (std::is_base_of_v<EnableSharedFromThis<T>, T>) {
            ptr->wptr = *this;
        }
    }

    SharedPtr(const SharedPtr& other) : ptr(other.ptr), cb(other.cb) {
        if (cb != nullptr) {
            ++cb->shared_counter;
            if constexpr (std::is_base_of_v<EnableSharedFromThis<T>, T>) {
                ptr->wptr = *this;
            }
        }
    }

    SharedPtr(SharedPtr&& other)
        : ptr(std::move(other.ptr)), cb(std::move(other.cb)) {
        other.ptr = nullptr;
        other.cb = nullptr;
        if (ptr != nullptr) {
            if constexpr (std::is_base_of_v<EnableSharedFromThis<T>, T>) {
                ptr->wptr = *this;
            }
        }
    }

    SharedPtr<T>& operator=(const SharedPtr& other) {
        if (this == &other) {
            return *this;
        }
        if (other.cb == cb || (other.ptr == ptr && ptr != nullptr)) {
            return *this;
        }
        if (cb != nullptr) {
            --cb->shared_counter;
            if (cb->shared_counter == 0) {
                cb->use_deleter();
                cb->call_destructor();
                if (cb->weak_counter == 0) {
                    cb->deallocate();
                    cb = nullptr;
                }
            }
        }
        ptr = other.ptr;
        cb = other.cb;
        if (cb != nullptr) {
            ++cb->shared_counter;
        }
        return *this;
    }

    template <typename U>
    SharedPtr<T>& operator=(SharedPtr<U>&& other) {
        if (cb != nullptr) {
            --cb->shared_counter;
            if (cb->shared_counter == 0) {
                cb->use_deleter();
                cb->call_destructor();
                if (cb->weak_counter == 0) {
                    cb->deallocate();
                    cb = nullptr;
                }
            }
        }
        ptr = other.ptr;
        other.ptr = nullptr;
        cb = std::move(other.cb);
        other.cb = nullptr;
        return *this;
    }

    SharedPtr& operator=(SharedPtr&& other) {
        if (this == &other) {
            return *this;
        }
        SharedPtr tmp = std::move(other);
        swap(tmp);
        return *this;
    }

    ~SharedPtr() {
        if (cb != nullptr) {
            --cb->shared_counter;
            if (cb->shared_counter == 0) {
                if (ptr != nullptr) {
                    cb->use_deleter();
                }
                cb->call_destructor();
                if (cb->weak_counter == 0) {
                    cb->deallocate();
                    cb = nullptr;
                }
            }
        }
    }

    T& operator*() const {
        return *get();
    }

    T* operator->() const {
        return get();
    }

    T* get() const {
        return ptr;
    }

    int use_count() const {
        if (cb == nullptr) {
            return 0;
        }
        return cb->shared_counter;
    }

    void swap(SharedPtr& other) {
        std::swap(cb, other.cb);
        std::swap(ptr, other.ptr);
    }

    template <typename Tp>
    void reset(Tp* new_ptr) {
        SharedPtr tmp(new_ptr);
        swap(tmp);
        return;
        if (cb == nullptr) {
            return;
        }
        --cb->shared_counter;
        if (cb->shared_counter == 0) {
            cb->use_deleter();
            cb->call_destructor();
            if (cb->weak_counter == 0) {
                cb->deallocate();
                cb = nullptr;
            }
        }
        ptr = nullptr;
        cb = new typename SharedPtr<Tp>::template ControlBlockMakeShared<>(
            1, 0, std::move(*new_ptr));
    }

    void reset() {
        SharedPtr tmp;
        swap(tmp);
    }
};

template <typename Tp, typename Alloc, typename... Args>
SharedPtr<Tp> allocateShared(const Alloc& alloc, Args&&... args) {
    using AllocControlBlock =
        typename std::allocator_traits<Alloc>::template rebind_alloc<
            typename SharedPtr<Tp>::template ControlBlockMakeShared<Alloc>>;
    AllocControlBlock alloc_cb = alloc;
    using traits = std::allocator_traits<AllocControlBlock>;
    typename SharedPtr<Tp>::template ControlBlockMakeShared<Alloc>* cb =
        traits::allocate(alloc_cb, 1);
    traits::construct(alloc_cb, cb, 1, 0, std::forward<Args>(args)..., alloc);
    auto* casted = static_cast<BaseControlBlock*>(cb);
    return SharedPtr<Tp>(casted);
}

template <typename Tp, typename... Args>
SharedPtr<Tp> makeShared(Args&&... args) {
    return allocateShared<Tp>(std::allocator<Tp>(),
                              std::forward<Args>(args)...);
}

template <typename T>
class WeakPtr {
    template <typename U>
    friend class SharedPtr;

    template <typename U>
    friend class WeakPtr;

  private:
    T* ptr;
    BaseControlBlock* cb;

  public:
    WeakPtr() {
        cb = nullptr;
        ptr = nullptr;
    }

    template <typename U>
    WeakPtr(const SharedPtr<U>& other) : ptr(other.ptr), cb(other.cb) {
        ++cb->weak_counter;
    }

    bool expired() const noexcept {
        return cb->shared_counter == 0;
    }

    SharedPtr<T> lock() const noexcept {
        if (!expired()) {
            ++cb->shared_counter;
            return SharedPtr<T>(cb);
        } else {
            return SharedPtr<T>();
        }
    }

    ~WeakPtr() {
        if (cb != nullptr) {
            --cb->weak_counter;
            if (cb->weak_counter == 0 && cb->shared_counter == 0) {
                cb->deallocate();
                cb = nullptr;
            }
        }
    }

    WeakPtr(const WeakPtr& other) : ptr(other.ptr), cb(other.cb) {
        ++cb->weak_counter;
    }

    template <typename U>
    WeakPtr(const WeakPtr<U>& other) : ptr(other.ptr), cb(other.cb) {
        ++cb->weak_counter;
    }

    WeakPtr(WeakPtr&& other) : ptr(other.ptr), cb(std::move(other.cb)) {
        other.ptr = nullptr;
        other.cb = nullptr;
    }

    void swap(WeakPtr& other) {
        std::swap(cb, other.cb);
        std::swap(ptr, other.ptr);
    }

    WeakPtr<T>& operator=(const WeakPtr& other) {
        if (this == &other) {
            return *this;
        }
        WeakPtr tmp = other;
        swap(tmp);
        return *this;
    }

    template <typename U>
    WeakPtr& operator=(const SharedPtr<U>& other) {
        WeakPtr tmp = other;
        swap(tmp);
        return *this;
    }

    WeakPtr& operator=(WeakPtr&& other) {
        WeakPtr tmp = std::move(other);
        swap(tmp);
        return this;
    }

    int use_count() const {
        if (cb == nullptr) {
            return 0;
        }
        return cb->shared_counter;
    }
};

template <typename T>
class EnableSharedFromThis {
    template <typename U>
    friend class SharedPtr;

  private:
    WeakPtr<T> wptr;

  public:
    SharedPtr<T> shared_from_this() const noexcept {
        return wptr.lock();
    }
};