#include <ppp/coroutines/YieldContext.h>

namespace ppp
{
    namespace coroutines
    {
        static constexpr int STATUS_RESUMED    = 0;
        static constexpr int STATUS_SUSPENDING = 1;
        static constexpr int STATUS_SUSPEND    = 2;
        static constexpr int STATUS_RESUMING   = -1;

        YieldContext::YieldContext(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, boost::asio::strand<boost::asio::io_context::executor_type>* strand, SpawnHander&& spawn, int stack_size) noexcept
            : s_(0)
            , callee_(NULL)
            , caller_(NULL)
            , h_(std::move(spawn))
            , context_(context)
            , strand_(strand)
            , stack_size_(stack_size)
            , allocator_(allocator)
        {
            if (allocator)
            {
                Byte* stack = (Byte*)allocator->Alloc(stack_size);
                if (stack)
                {
                    stack_ = std::shared_ptr<Byte>(stack, /* std::bind(&ppp::threading::BufferswapAllocator::Free, allocator, std::placeholders::_1)); */
                        [allocator](Byte* p) noexcept 
                        {
                            allocator->Free(p);
                        });
                }
            }

            /* boost::context::stack_traits::minimum_size(); */
            if (!stack_)
            {
                stack_ = make_shared_alloc<Byte>(stack_size);
            }
        }

        YieldContext::~YieldContext() noexcept
        {
            YieldContext* y = this;
            y->h_          = NULL;
            y->stack_      = NULL;
            y->stack_size_ = 0;
            y->strand_     = NULL;
            y->allocator_  = NULL;
        }

#if defined(_WIN32)
#pragma optimize("", off)
#pragma optimize("gsyb2", on) /* /O1 = /Og /Os /Oy /Ob2 /GF /Gy */
#else
// TRANSMISSIONO1 compiler macros are defined to perform O1 optimizations, 
// Otherwise gcc compiler version If <= 7.5.X, 
// The O1 optimization will also be applied, 
// And the other cases will not be optimized, 
// Because this will cause the program to crash, 
// Which is a fatal BUG caused by the gcc compiler optimization. 
// Higher-version compilers should not optimize the code for gcc compiling this section.
#if defined(__clang__)
#pragma clang optimize off
#else
#pragma GCC push_options
#if defined(TRANSMISSION_O1) || (__GNUC__ < 7) || (__GNUC__ == 7 && __GNUC_MINOR__ <= 5) /* __GNUC_PATCHLEVEL__ */
#pragma GCC optimize("O1")
#else
#pragma GCC optimize("O0")
#endif
#endif
#endif
        bool YieldContext::Suspend() noexcept
        {
            int L = STATUS_RESUMED;
            if (s_.compare_exchange_strong(L, STATUS_SUSPENDING))
            {
                YieldContext* y = this;
                y->caller_.exchange(
                    boost::context::detail::jump_fcontext(
                        y->caller_.exchange(NULL), y).fctx);

                L = STATUS_RESUMING;
                return y->s_.compare_exchange_strong(L, STATUS_RESUMED);
            }
            else
            {
                return false;
            }
        }

        bool YieldContext::Resume() noexcept
        {
            int L = STATUS_SUSPEND;
            if (s_.compare_exchange_strong(L, STATUS_RESUMING))
            {
                YieldContext* y = this;
                return Switch(
                    boost::context::detail::jump_fcontext(
                        y->callee_.exchange(NULL), y), y);
            }
            else
            {
                return false;
            }
        }

        void YieldContext::Invoke() noexcept
        {
            YieldContext* y = this;
            Byte* stack = stack_.get(); 

            if (stack)
            {
                boost::context::detail::fcontext_t callee =
                    boost::context::detail::make_fcontext(stack + stack_size_, stack_size_, &YieldContext::Handle);
                Switch(boost::context::detail::jump_fcontext(callee, y), y);
            }
            else
            {
                YieldContext::Release(y);
            }
        }

        boost::context::detail::transfer_t YieldContext::Jump(boost::context::detail::fcontext_t context, void* state) noexcept
        {
            if (context) 
            {
                return boost::context::detail::jump_fcontext(context, state);
            }

            return boost::context::detail::transfer_t{ NULL, NULL };
        }
#if defined(_WIN32)
#pragma optimize("", on)
#else
#if defined(__clang__)
#pragma clang optimize on
#else
#pragma GCC pop_options
#endif
#endif

        bool YieldContext::Switch() noexcept(false)
        {
            int L = STATUS_SUSPENDING;
            if (s_.compare_exchange_strong(L, STATUS_SUSPEND))
            {
                return true;
            }
            
            throw std::runtime_error("The internal atomic state used for the yield_context switch was corrupted..");
        }

        bool YieldContext::Switch(const boost::context::detail::transfer_t& t, YieldContext* y) noexcept
        {
            if (t.data)
            {
                y->callee_.exchange(t.fctx);
                return y->Switch();
            }
            else
            {
                YieldContext::Release(y);
                return true;
            }
        }

        void YieldContext::Handle(boost::context::detail::transfer_t t) noexcept(false)
        {
            YieldContext* y = (YieldContext*)t.data;
            if (y)
            {
                SpawnHander h = std::move(y->h_);
                y->h_ = NULL;
                y->caller_.exchange(t.fctx);

                if (h)
                {
                    h(*y);
                    h = NULL;
                }

                Jump(y->caller_.exchange(NULL), NULL);
                if (y->callee_.exchange(NULL))
                {
                    throw std::runtime_error("The yield_context has a serious abnormal handover exit problem.");
                }
            }
        }
 
        bool YieldContext::Spawn(ppp::threading::BufferswapAllocator* allocator, boost::asio::io_context& context, boost::asio::strand<boost::asio::io_context::executor_type>* strand, SpawnHander&& spawn, int stack_size) noexcept
        {
            if (!spawn)
            {
                return false;
            }

            int pagesize = GetMemoryPageSize();
            stack_size = std::max<int>(stack_size, pagesize);

            // If done on the thread that owns the context, it is executed immediately.
            // Otherwise, the delivery event is delivered to the actor queue of the context, 
            // And the host thread of the context drives it when the next event is triggered.
            YieldContext* y = New<YieldContext>(allocator, context, strand, std::move(spawn), stack_size);
            if (!y)
            {
                return false;
            }

            // By default the C/C++ compiler optimizes the context delegate event call, and strand is usually multi-core driven if it occurs.
            auto invoked =
                [y]() noexcept -> void
                {
                    y->Invoke();
                };

            if (strand)
            {
                boost::asio::post(*strand, invoked);
            }
            else
            {
                context.post(invoked);
            }

            return true;
        }

        bool YieldContext::R() noexcept
        {
            YieldContext* y = this;
            auto invoked =
                [y]() noexcept -> void
                {
                    bool resumed = y->Resume();
                    if (!resumed)
                    {
                        y->R();
                    }
                };

            boost::asio::io_context* context = &y->context_;
            return ppp::threading::Executors::Post(context, y->strand_, invoked);
        }
    }
}