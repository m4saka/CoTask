﻿//----------------------------------------------------------------------------------------
//
//  CoTaskLib
//
//  Copyright (c) 2024 masaka
//
//  Licensed under the MIT License.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//  SOFTWARE.
//
//----------------------------------------------------------------------------------------

#pragma once
#include <Siv3D.hpp>
#include <coroutine>

namespace cotasklib
{
	namespace Co
	{
		namespace detail
		{
			class IAwaiter
			{
			public:
				virtual ~IAwaiter() = default;

				virtual void resume() = 0;

				[[nodiscard]]
				virtual bool done() const = 0;
			};

			struct AwaiterEntry
			{
				std::unique_ptr<IAwaiter> awaiter;
				std::function<void(const IAwaiter*)> finishCallback;
				std::function<void()> cancelCallback;

				void callEndCallback() const
				{
					if (awaiter->done())
					{
						if (finishCallback)
						{
							finishCallback(awaiter.get());
						}
					}
					else
					{
						if (cancelCallback)
						{
							cancelCallback();
						}
					}
				}
			};

			using AwaiterID = uint64;

			using UpdaterID = uint64;

			using UpdateInputCallerID = uint64;

			using DrawerID = uint64;

			template <typename TResult>
			class TaskAwaiter;

			template <typename TResult>
			struct FinishCallbackTypeTrait
			{
				using type = std::function<void(TResult)>;
			};

			template <>
			struct FinishCallbackTypeTrait<void>
			{
				using type = std::function<void()>;
			};
		}

		template <typename TResult>
		using FinishCallbackType = typename detail::FinishCallbackTypeTrait<TResult>::type;

		template <typename TResult>
		class Task;

		class SceneBase;

		using SceneFactory = std::function<std::unique_ptr<SceneBase>()>;

		namespace detail
		{
			template <typename IDType>
			class OrderedExecutor
			{
			private:
				struct CallerKey
				{
					IDType id;
					int32 sortingOrder;

					CallerKey(IDType id, int32 sortingOrder)
						: id(id)
						, sortingOrder(sortingOrder)
					{
					}

					CallerKey(const CallerKey&) noexcept = default;

					CallerKey& operator=(const CallerKey&) noexcept = default;

					CallerKey(CallerKey&&) noexcept = default;

					CallerKey& operator=(CallerKey&&) noexcept = default;

					bool operator<(const CallerKey& other) const noexcept
					{
						if (sortingOrder != other.sortingOrder)
						{
							return sortingOrder < other.sortingOrder;
						}
						return id < other.id;
					}
				};

				struct Caller
				{
					std::function<void()> func;
					std::function<int32()> sortingOrderFunc;

					Caller(std::function<void()> func, std::function<int32()> sortingOrderFunc)
						: func(std::move(func))
						, sortingOrderFunc(std::move(sortingOrderFunc))
					{
					}

					Caller(const Caller&) = default;

					Caller& operator=(const Caller&) = default;
				};

				IDType m_nextID = 1;
				std::map<CallerKey, Caller> m_callers;
				std::unordered_map<IDType, CallerKey> m_callerKeyByID;
				std::vector<std::pair<CallerKey, int32>> m_tempNewSortingOrders;

				using CallersIterator = typename decltype(m_callers)::iterator;

				void refreshSortingOrder()
				{
					m_tempNewSortingOrders.clear();

					// まず、sortingOrderの変更をリストアップ
					for (const auto& [key, caller] : m_callers)
					{
						const int32 newSortingOrder = caller.sortingOrderFunc();
						if (newSortingOrder != key.sortingOrder)
						{
							m_tempNewSortingOrders.emplace_back(key, newSortingOrder);
						}
					}

					// sortingOrderに変更があったものを再挿入
					for (const auto& [oldKey, newSortingOrder] : m_tempNewSortingOrders)
					{
						const IDType id = oldKey.id;
						const auto it = m_callers.find(oldKey);
						if (it == m_callers.end())
						{
							throw Error{ U"OrderedExecutor::refreshSortingOrder: ID={} not found"_fmt(id) };
						}
						Caller newCaller = it->second;
						m_callers.erase(it);
						const auto [newIt, inserted] = m_callers.insert(std::make_pair(CallerKey{ id, newSortingOrder }, std::move(newCaller)));
						if (!inserted)
						{
							throw Error{ U"OrderedExecutor::refreshSortingOrder: ID={} cannot be inserted"_fmt(id) };
						}
						m_callerKeyByID.insert_or_assign(id, newIt->first);
					}
				}

			public:
				IDType add(std::function<void()> func, std::function<int32()> sortingOrderFunc)
				{
					const int32 sortingOrder = sortingOrderFunc();
					const auto [it, inserted] = m_callers.insert(std::make_pair(CallerKey{ m_nextID, sortingOrder }, Caller{ std::move(func), std::move(sortingOrderFunc) }));
					if (!inserted)
					{
						throw Error{ U"OrderedExecutor::add: ID={} cannot be inserted"_fmt(m_nextID) };
					}
					if (m_callerKeyByID.contains(m_nextID))
					{
						throw Error{ U"OrderedExecutor::add: ID={} inconsistency detected"_fmt(m_nextID) };
					}
					m_callerKeyByID.insert_or_assign(m_nextID, it->first);
					return m_nextID++;
				}

				CallersIterator findByID(IDType id)
				{
					const auto it = m_callerKeyByID.find(id);
					if (it == m_callerKeyByID.end())
					{
						return m_callers.end();
					}
					return m_callers.find(it->second);
				}

				void remove(IDType id)
				{
					const auto it = findByID(id);
					if (it == m_callers.end())
					{
						if (m_callerKeyByID.contains(id))
						{
							throw Error{ U"OrderedExecutor::remove: ID={} inconsistency detected"_fmt(id) };
						}
						return;
					}
					m_callers.erase(it);
					m_callerKeyByID.erase(id);
				}

				void call()
				{
					refreshSortingOrder();

					for (const auto& [key, caller] : m_callers)
					{
						caller.func();
					}
				}

				[[nodiscard]]
				bool hasSortingOrder(int32 sortingOrder) const
				{
					for (const auto& [key, caller] : m_callers)
					{
						if (caller.sortingOrderFunc() == sortingOrder)
						{
							return true;
						}
					}
					return false;
				}

				[[nodiscard]]
				bool hasSortingOrderInRange(int32 sortingOrderMin, int32 sortingOrderMax) const
				{
					for (const auto& [key, caller] : m_callers)
					{
						const int32 sortingOrder = caller.sortingOrderFunc();
						if (sortingOrderMin <= sortingOrder && sortingOrder <= sortingOrderMax)
						{
							return true;
						}
					}
					return false;
				}
			};

			class Backend
			{
			private:
				static constexpr StringView AddonName{ U"Co::BackendAddon" };

				static inline Backend* s_pInstance = nullptr;

				// Note: draw関数がconstであることの対処用にアドオンと実体を分離し、実体はポインタで持つようにしている
				class BackendAddon : public IAddon
				{
				private:
					std::unique_ptr<Backend> m_instance;

				public:
					BackendAddon()
						: m_instance{ std::make_unique<Backend>() }
					{
						if (s_pInstance)
						{
							throw Error{ U"Co::BackendAddon: Instance already exists" };
						}
						s_pInstance = m_instance.get();
					}

					virtual ~BackendAddon()
					{
						if (s_pInstance == m_instance.get())
						{
							s_pInstance = nullptr;
						}
					}

					virtual bool update() override
					{
						m_instance->update();
						return true;
					}

					virtual void draw() const override
					{
						m_instance->draw();
					}

					[[nodiscard]]
					Backend* instance() const
					{
						return m_instance.get();
					}
				};

				AwaiterID m_nextAwaiterID = 1;

				Optional<AwaiterID> m_currentAwaiterID = none;

				bool m_currentAwaiterRemovalNeeded = false;

				std::map<AwaiterID, AwaiterEntry> m_awaiterEntries;

				OrderedExecutor<UpdateInputCallerID> m_updateInputExecutor;

				OrderedExecutor<DrawerID> m_drawExecutor;

				SceneFactory m_currentSceneFactory;

			public:
				Backend() = default;

				void update()
				{
					std::exception_ptr exceptionPtr;
					for (auto it = m_awaiterEntries.begin(); it != m_awaiterEntries.end();)
					{
						m_currentAwaiterID = it->first;

						const auto& entry = it->second;
						entry.awaiter->resume();
						if (m_currentAwaiterRemovalNeeded || entry.awaiter->done())
						{
							try
							{
								entry.callEndCallback();
							}
							catch (...)
							{
								if (!exceptionPtr)
								{
									exceptionPtr = std::current_exception();
								}
							}
							it = m_awaiterEntries.erase(it);
							m_currentAwaiterRemovalNeeded = false;
						}
						else
						{
							++it;
						}
					}
					m_currentAwaiterID.reset();
					if (exceptionPtr)
					{
						std::rethrow_exception(exceptionPtr);
					}
				}

				void draw()
				{
					m_updateInputExecutor.call();
					m_drawExecutor.call();
				}

				static void Init()
				{
					Addon::Register(AddonName, std::make_unique<BackendAddon>());
				}

				template <typename TResult>
				[[nodiscard]]
				static AwaiterID Add(std::unique_ptr<TaskAwaiter<TResult>>&& awaiter, FinishCallbackType<TResult> finishCallback, std::function<void()> cancelCallback)
				{
					if (!awaiter)
					{
						throw Error{ U"awaiter must not be nullptr" };
					}

					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}
					const AwaiterID id = s_pInstance->m_nextAwaiterID++;
					std::function<void(const IAwaiter*)> finishCallbackTypeErased =
						[finishCallback = std::move(finishCallback), cancelCallback/*コピーキャプチャ*/, id](const IAwaiter* awaiter)
						{
							auto fnGetResult = [awaiter, cancelCallback, id]() -> TResult
								{
									try
									{
										// awaiterの型がTaskAwaiter<TResult>であることは保証されるため、static_castでキャストして問題ない
										return static_cast<const TaskAwaiter<TResult>*>(awaiter)->value();
									}
									catch (...)
									{
										// 例外を捕捉した場合はキャンセル扱いにした上で例外を投げ直す
										if (cancelCallback)
										{
											cancelCallback();
										}
										throw;
									}
								};
							if constexpr (std::is_void_v<TResult>)
							{
								fnGetResult(); // 例外伝搬のためにvoidでも呼び出す
								if (finishCallback)
								{
									finishCallback();
								}
							}
							else
							{
								auto result = fnGetResult();
								if (finishCallback)
								{
									finishCallback(std::move(result));
								}
							}
						};
					s_pInstance->m_awaiterEntries.emplace(id,
						AwaiterEntry
						{
							.awaiter = std::move(awaiter),
							.finishCallback = std::move(finishCallbackTypeErased),
							.cancelCallback = std::move(cancelCallback),
						});
					return id;
				}

				static void Remove(AwaiterID id)
				{
					if (!s_pInstance)
					{
						// Note: ユーザーがインスタンスをstaticで持ってしまった場合にAddon解放後に呼ばれるケースが起こりうるので、ここでは例外を出さない
						return;
					}
					if (id == s_pInstance->m_currentAwaiterID)
					{
						// 実行中タスクのAwaiterをここで削除するとアクセス違反やイテレータ破壊が起きるため、代わりに削除フラグを立てて実行完了時に削除
						// (例えば、タスク実行のライフタイムをOptional<ScopedTaskRunner>型のメンバ変数として持ち、タスク実行中にそこへnoneを代入して実行を止める場合が該当)
						s_pInstance->m_currentAwaiterRemovalNeeded = true;
						return;
					}
					const auto it = s_pInstance->m_awaiterEntries.find(id);
					if (it != s_pInstance->m_awaiterEntries.end())
					{
						it->second.callEndCallback();
						s_pInstance->m_awaiterEntries.erase(it);
					}
				}

				[[nodiscard]]
				static bool IsDone(AwaiterID id)
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}
					const auto it = s_pInstance->m_awaiterEntries.find(id);
					if (it != s_pInstance->m_awaiterEntries.end())
					{
						return it->second.awaiter->done();
					}
					return id < s_pInstance->m_nextAwaiterID;
				}

				static void ManualUpdate()
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}
					s_pInstance->update();
				}

				[[nodiscard]]
				static UpdateInputCallerID AddUpdateInputCaller(std::function<void()> func, std::function<int32()> negativeDrawIndexFunc)
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}
					return s_pInstance->m_updateInputExecutor.add(std::move(func), std::move(negativeDrawIndexFunc));
				}

				static void RemoveUpdateInputCaller(UpdateInputCallerID id)
				{
					if (!s_pInstance)
					{
						// Note: ユーザーがインスタンスをstaticで持ってしまった場合にAddon解放後に呼ばれるケースが起こりうるので、ここでは例外を出さない
						return;
					}
					s_pInstance->m_updateInputExecutor.remove(id);
				}

				[[nodiscard]]
				static DrawerID AddDrawer(std::function<void()> func, std::function<int32()> drawIndexFunc)
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}
					return s_pInstance->m_drawExecutor.add(std::move(func), std::move(drawIndexFunc));
				}

				static void RemoveDrawer(DrawerID id)
				{
					if (!s_pInstance)
					{
						// Note: ユーザーがインスタンスをstaticで持ってしまった場合にAddon解放後に呼ばれるケースが起こりうるので、ここでは例外を出さない
						return;
					}
					s_pInstance->m_drawExecutor.remove(id);
				}

				[[nodiscard]]
				static bool HasActiveDrawer(int32 drawIndex)
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}

					return s_pInstance->m_drawExecutor.hasSortingOrder(drawIndex);
				}

				[[nodiscard]]
				static bool HasActiveDrawerInRange(int32 drawIndexMin, int32 drawIndexMax)
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}

					return s_pInstance->m_drawExecutor.hasSortingOrderInRange(drawIndexMin, drawIndexMax);
				}

				[[nodiscard]]
				static void SetCurrentSceneFactory(SceneFactory factory)
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}
					s_pInstance->m_currentSceneFactory = std::move(factory);
				}

				[[nodiscard]]
				static SceneFactory CurrentSceneFactory()
				{
					if (!s_pInstance)
					{
						throw Error{ U"Backend is not initialized" };
					}
					return s_pInstance->m_currentSceneFactory;
				}
			};

			template <typename TResult>
			[[nodiscard]]
			Optional<AwaiterID> ResumeAwaiterOnceAndRegisterIfNotDone(TaskAwaiter<TResult>&& awaiter, FinishCallbackType<TResult> finishCallback, std::function<void()> cancelCallback)
			{
				const auto fnCallFinishCallback = [&]
					{
						if constexpr (std::is_void_v<TResult>)
						{
							try
							{
								awaiter.value(); // 例外伝搬のためにvoidでも呼び出す
							}
							catch (...)
							{
								if (cancelCallback)
								{
									cancelCallback();
								}
								throw;
							}

							if (finishCallback)
							{
								finishCallback();
							}
						}
						else
						{
							auto result = [&]() -> TResult
								{
									try
									{
										return awaiter.value();
									}
									catch (...)
									{
										if (cancelCallback)
										{
											cancelCallback();
										}
										throw;
									}
								}();
							if (finishCallback)
							{
								finishCallback(std::move(result));
							}
						}
					};

				// フレーム待ちなしで終了した場合は登録不要
				// (ここで一度resumeするのは、runScoped実行まで開始を遅延させるためにinitial_suspendをsuspend_alwaysにしているため)
				if (awaiter.done())
				{
					fnCallFinishCallback();
					return none;
				}
				awaiter.resume();
				if (awaiter.done())
				{
					fnCallFinishCallback();
					return none;
				}

				return Backend::Add(std::make_unique<TaskAwaiter<TResult>>(std::move(awaiter)), std::move(finishCallback), std::move(cancelCallback));
			}

			template <typename TResult>
			Optional<AwaiterID> ResumeAwaiterOnceAndRegisterIfNotDone(const TaskAwaiter<TResult>& awaiter) = delete;
		}

		[[nodiscard]]
		inline auto NextFrame() noexcept
		{
			return std::suspend_always{};
		}

		class MultiRunner;

		class ScopedTaskRunner
		{
		private:
			Optional<detail::AwaiterID> m_id;

		public:
			template <typename TResult>
			explicit ScopedTaskRunner(Task<TResult>&& task, FinishCallbackType<TResult> finishCallback = nullptr, std::function<void()> cancelCallback = nullptr)
				: m_id(ResumeAwaiterOnceAndRegisterIfNotDone(detail::TaskAwaiter<TResult>{ std::move(task) }, std::move(finishCallback), std::move(cancelCallback)))
			{
			}

			ScopedTaskRunner(const ScopedTaskRunner&) = delete;

			ScopedTaskRunner& operator=(const ScopedTaskRunner&) = delete;

			ScopedTaskRunner(ScopedTaskRunner&& rhs) noexcept
				: m_id(rhs.m_id)
			{
				rhs.m_id.reset();
			}

			ScopedTaskRunner& operator=(ScopedTaskRunner&&) = delete;

			~ScopedTaskRunner()
			{
				if (m_id.has_value())
				{
					detail::Backend::Remove(*m_id);
				}
			}

			[[nodiscard]]
			bool done() const
			{
				return !m_id.has_value() || detail::Backend::IsDone(*m_id);
			}

			void forget()
			{
				m_id.reset();
			}

			void requestCancel()
			{
				if (m_id.has_value())
				{
					detail::Backend::Remove(*m_id);
					m_id.reset();
				}
			}

			void addTo(MultiRunner& mr)&&;

			[[nodiscard]]
			Task<void> waitUntilDone() const&;

			[[nodiscard]]
			Task<void> waitUntilDone() const&& = delete;
		};

		class MultiRunner
		{
		private:
			std::vector<ScopedTaskRunner> m_runners;

		public:
			MultiRunner() = default;

			MultiRunner(const MultiRunner&) = delete;

			MultiRunner& operator=(const MultiRunner&) = delete;

			MultiRunner(MultiRunner&&) = default;

			MultiRunner& operator=(MultiRunner&&) = default;

			~MultiRunner() = default;

			void add(ScopedTaskRunner&& runner)
			{
				m_runners.push_back(std::move(runner));
			}

			void reserve(std::size_t size)
			{
				m_runners.reserve(size);
			}

			void clear()
			{
				m_runners.clear();
			}

			void requestCancelAll()
			{
				for (ScopedTaskRunner& runner : m_runners)
				{
					runner.requestCancel();
				}
			}

			[[nodiscard]]
			bool allDone() const
			{
				return std::ranges::all_of(m_runners, [](const ScopedTaskRunner& runner) { return runner.done(); });
			}

			[[nodiscard]]
			bool anyDone() const
			{
				return std::ranges::any_of(m_runners, [](const ScopedTaskRunner& runner) { return runner.done(); });
			}

			[[nodiscard]]
			Task<void> waitUntilAllDone() const&;

			[[nodiscard]]
			Task<void> waitUntilAllDone() const&& = delete;

			[[nodiscard]]
			Task<void> waitUntilAnyDone() const&;

			[[nodiscard]]
			Task<void> waitUntilAnyDone() const&& = delete;
		};

		inline void ScopedTaskRunner::addTo(MultiRunner& mr)&&
		{
			mr.add(std::move(*this));
		}

		namespace DrawIndex
		{
			constexpr int32 Back = -1;
			constexpr int32 Default = 0;
			constexpr int32 Front = 1;

			constexpr int32 ModalMin = 100000;
			constexpr int32 ModalBack = 149999;
			constexpr int32 Modal = 150000;
			constexpr int32 ModalFront = 150001;
			constexpr int32 ModalMax = 199999;

			constexpr int32 FadeInMin = 200000;
			constexpr int32 FadeInBack = 249999;
			constexpr int32 FadeIn = 250000;
			constexpr int32 FadeInFront = 250001;
			constexpr int32 FadeInMax = 299999;

			constexpr int32 FadeOutMin = 300000;
			constexpr int32 FadeOutBack = 349999;
			constexpr int32 FadeOut = 350000;
			constexpr int32 FadeOutFront = 350001;
			constexpr int32 FadeOutMax = 399999;
		}

		class ScopedUpdateInputCaller
		{
		private:
			Optional<detail::UpdateInputCallerID> m_id;

		public:
			ScopedUpdateInputCaller(std::function<void()> func, std::function<int32()> negativeDrawIndexFunc)
				: m_id(detail::Backend::AddUpdateInputCaller(std::move(func), std::move(negativeDrawIndexFunc)))
			{
			}

			ScopedUpdateInputCaller(const ScopedUpdateInputCaller&) = delete;

			ScopedUpdateInputCaller& operator=(const ScopedUpdateInputCaller&) = delete;

			ScopedUpdateInputCaller(ScopedUpdateInputCaller&& rhs) noexcept
				: m_id(rhs.m_id)
			{
				rhs.m_id.reset();
			}

			ScopedUpdateInputCaller& operator=(ScopedUpdateInputCaller&& rhs) = delete;

			~ScopedUpdateInputCaller()
			{
				if (m_id.has_value())
				{
					detail::Backend::RemoveUpdateInputCaller(*m_id);
				}
			}
		};

		class ScopedDrawer
		{
		private:
			Optional<detail::DrawerID> m_id;

		public:
			ScopedDrawer(std::function<void()> func)
				: m_id(detail::Backend::AddDrawer(std::move(func), [] { return DrawIndex::Default; }))
			{
			}

			ScopedDrawer(std::function<void()> func, int32 drawIndex)
				: m_id(detail::Backend::AddDrawer(std::move(func), [drawIndex] { return drawIndex; }))
			{
			}

			ScopedDrawer(std::function<void()> func, std::function<int32()> drawIndexFunc)
				: m_id(detail::Backend::AddDrawer(std::move(func), std::move(drawIndexFunc)))
			{
			}

			ScopedDrawer(const ScopedDrawer&) = delete;

			ScopedDrawer& operator=(const ScopedDrawer&) = delete;

			ScopedDrawer(ScopedDrawer&& rhs) noexcept
				: m_id(rhs.m_id)
			{
				rhs.m_id.reset();
			}

			ScopedDrawer& operator=(ScopedDrawer&& rhs) = delete;

			~ScopedDrawer()
			{
				if (m_id.has_value())
				{
					detail::Backend::RemoveDrawer(*m_id);
				}
			}
		};

		namespace detail
		{
			template <typename TResult>
			class Promise;

			template <typename TResult>
			class CoroutineHandleWrapper
			{
			private:
				using handle_type = std::coroutine_handle<Promise<TResult>>;

				handle_type m_handle;

			public:
				explicit CoroutineHandleWrapper(handle_type handle)
					: m_handle(std::move(handle))
				{
				}

				~CoroutineHandleWrapper()
				{
					if (m_handle)
					{
						m_handle.destroy();
					}
				}

				CoroutineHandleWrapper(const CoroutineHandleWrapper<TResult>&) = delete;

				CoroutineHandleWrapper& operator=(const CoroutineHandleWrapper<TResult>&) = delete;

				CoroutineHandleWrapper(CoroutineHandleWrapper<TResult>&& rhs) noexcept
					: m_handle(rhs.m_handle)
				{
					rhs.m_handle = nullptr;
				}

				CoroutineHandleWrapper& operator=(CoroutineHandleWrapper<TResult>&& rhs) = delete;

				[[nodiscard]]
				TResult value() const
				{
					return m_handle.promise().value();
				}

				[[nodiscard]]
				bool done() const
				{
					return !m_handle || m_handle.done();
				}

				void resume() const
				{
					if (done())
					{
						return;
					}

					if (m_handle.promise().resumeSubAwaiter())
					{
						return;
					}

					m_handle.resume();
				}
			};

			template <typename TResult>
			[[nodiscard]]
			Task<void> DiscardResult(std::unique_ptr<Task<TResult>> task)
			{
				co_await std::move(*task);
			}
		}

		enum class WithTiming
		{
			// with実行タイミングで既に初回resumeは既に完了してしまっているので、
			// もし初回resumeも前に実行する必要がある場合は、withに渡す子Taskを
			// 親Taskより先に生成し、withへ子タスクをstd::moveで渡す必要がある点に注意
			Before,

			After,
		};

		class [[nodiscard]] ITask
		{
		public:
			virtual ~ITask() = default;

			virtual void resume() = 0;

			[[nodiscard]]
			virtual bool done() const = 0;
		};

		template <typename TResult = void>
		class [[nodiscard]] Task : public ITask
		{
			static_assert(!std::is_reference_v<TResult>, "TResult must not be a reference type");
			static_assert(std::is_move_constructible_v<TResult> || std::is_void_v<TResult>, "TResult must be move constructible");
			static_assert(!std::is_const_v<TResult>, "TResult must not have 'const' qualifier");

		private:
			detail::CoroutineHandleWrapper<TResult> m_handle;
			std::vector<std::unique_ptr<ITask>> m_concurrentTasksBefore;
			std::vector<std::unique_ptr<ITask>> m_concurrentTasksAfter;

		public:
			using promise_type = detail::Promise<TResult>;
			using handle_type = std::coroutine_handle<promise_type>;
			using result_type = TResult;
			using finish_callback_type = FinishCallbackType<TResult>;

			explicit Task(handle_type h)
				: m_handle(std::move(h))
			{
			}

			Task(const Task<TResult>&) = delete;

			Task<TResult>& operator=(const Task<TResult>&) = delete;

			Task(Task<TResult>&& rhs) noexcept = default;

			Task<TResult>& operator=(Task<TResult>&& rhs) = delete;

			virtual void resume()
			{
				if (m_handle.done())
				{
					return;
				}

				for (auto& task : m_concurrentTasksBefore)
				{
					task->resume();
				}

				m_handle.resume();

				for (auto& task : m_concurrentTasksAfter)
				{
					task->resume();
				}
			}

			[[nodiscard]]
			bool done() const override
			{
				return m_handle.done();
			}

			[[nodiscard]]
			TResult value() const
			{
				return m_handle.value();
			}

			template <typename TResultOther>
			[[nodiscard]]
			Task<TResult> with(Task<TResultOther>&& task)&&
			{
				m_concurrentTasksAfter.push_back(std::make_unique<Task<TResultOther>>(std::move(task)));
				return std::move(*this);
			}

			template <typename TResultOther>
			[[nodiscard]]
			Task<TResult> with(Task<TResultOther>&& task, WithTiming timing)&&
			{
				switch (timing)
				{
				case WithTiming::Before:
					m_concurrentTasksBefore.push_back(std::make_unique<Task<TResultOther>>(std::move(task)));
					break;

				case WithTiming::After:
					m_concurrentTasksAfter.push_back(std::make_unique<Task<TResultOther>>(std::move(task)));
					break;

				default:
					throw Error{ U"Task: Invalid WithTiming" };
				}
				return std::move(*this);
			}

			[[nodiscard]]
			Task<void> discardResult()&&
			{
				return detail::DiscardResult(std::make_unique<Task<TResult>>(std::move(*this)));
			}

			[[nodiscard]]
			ScopedTaskRunner runScoped(FinishCallbackType<TResult> finishCallback = nullptr, std::function<void()> cancelCallback = nullptr)&&
			{
				return ScopedTaskRunner{ std::move(*this), std::move(finishCallback), std::move(cancelCallback) };
			}
			
			[[nodiscard]]
			void runAddTo(MultiRunner& mr, FinishCallbackType<TResult> finishCallback = nullptr, std::function<void()> cancelCallback = nullptr)&&
			{
				mr.add(ScopedTaskRunner{ std::move(*this), std::move(finishCallback), std::move(cancelCallback) });
			}
		};

		namespace detail
		{
			template <typename TResult>
			class [[nodiscard]] TaskAwaiter : public detail::IAwaiter
			{
				static_assert(!std::is_reference_v<TResult>, "TResult must not be a reference type");
				static_assert(std::is_move_constructible_v<TResult> || std::is_void_v<TResult>, "TResult must be move constructible");
				static_assert(!std::is_const_v<TResult>, "TResult must not have 'const' qualifier");

			private:
				Task<TResult> m_task;

			public:
				explicit TaskAwaiter(Task<TResult>&& task)
					: m_task(std::move(task))
				{
				}

				TaskAwaiter(const TaskAwaiter<TResult>&) = delete;

				TaskAwaiter<TResult>& operator=(const TaskAwaiter<TResult>&) = delete;

				TaskAwaiter(TaskAwaiter<TResult>&& rhs) noexcept = default;

				TaskAwaiter<TResult>& operator=(TaskAwaiter<TResult>&& rhs) = delete;

				void resume() override
				{
					m_task.resume();
				}

				[[nodiscard]]
				bool done() const override
				{
					return m_task.done();
				}

				[[nodiscard]]
				bool await_ready()
				{
					return m_task.done();
				}

				template <typename TResultOther>
				bool await_suspend(std::coroutine_handle<detail::Promise<TResultOther>> handle)
				{
					resume();
					if (m_task.done())
					{
						// フレーム待ちなしで終了した場合は登録不要
						return false;
					}
					handle.promise().setSubAwaiter(this);
					return true;
				}

				TResult await_resume() const
				{
					return m_task.value();
				}

				[[nodiscard]]
				TResult value() const
				{
					return m_task.value();
				}
			};

			class PromiseBase
			{
			protected:
				IAwaiter* m_pSubAwaiter = nullptr;

			public:
				PromiseBase() = default;

				PromiseBase(const PromiseBase&) = delete;

				PromiseBase& operator=(const PromiseBase&) = delete;

				PromiseBase(PromiseBase&& rhs) noexcept
					: m_pSubAwaiter(rhs.m_pSubAwaiter)
				{
					rhs.m_pSubAwaiter = nullptr;
				}

				PromiseBase& operator=(PromiseBase&& rhs) = delete;

				virtual ~PromiseBase() = 0;

				[[nodiscard]]
				auto initial_suspend() noexcept
				{
					// suspend_neverにすれば関数呼び出し時点で実行開始されるが、
					// その場合Co::AllやCo::Anyに渡す際など引数の評価順が不定になり扱いづらいため、
					// ここではsuspend_alwaysにしてrunScoped実行まで実行を遅延させている
					return std::suspend_always{};
				}

				[[nodiscard]]
				auto final_suspend() noexcept
				{
					return std::suspend_always{};
				}

				[[nodiscard]]
				bool resumeSubAwaiter()
				{
					if (!m_pSubAwaiter)
					{
						return false;
					}

					m_pSubAwaiter->resume();

					if (m_pSubAwaiter->done())
					{
						m_pSubAwaiter = nullptr;
						return false;
					}

					return true;
				}

				void setSubAwaiter(IAwaiter* pSubAwaiter) noexcept
				{
					m_pSubAwaiter = pSubAwaiter;
				}
			};

			inline PromiseBase::~PromiseBase() = default;

			template <typename TResult>
			class Promise : public PromiseBase
			{
				static_assert(!std::is_reference_v<TResult>, "TResult must not be a reference type");
				static_assert(std::is_move_constructible_v<TResult> || std::is_void_v<TResult>, "TResult must be move constructible");
				static_assert(!std::is_const_v<TResult>, "TResult must not have 'const' qualifier");

			private:
				std::promise<TResult> m_value;
				bool m_isResultSet = false;
				bool m_resultConsumed = false;

			public:
				Promise() = default;

				Promise(Promise<TResult>&&) noexcept = default;

				Promise& operator=(Promise<TResult>&&) = delete;

				void return_value(const TResult& v) requires std::is_copy_constructible_v<TResult>
				{
					m_value.set_value(v);
					m_isResultSet = true;
				}

				void return_value(TResult&& v)
				{
					m_value.set_value(std::move(v));
					m_isResultSet = true;
				}

				[[nodiscard]]
				TResult value()
				{
					if (!m_isResultSet)
					{
						throw Error{ U"Task is not completed. Make sure that all paths in the coroutine return a value." };
					}
					if (m_resultConsumed)
					{
						throw Error{ U"Task result can be get only once." };
					}
					m_resultConsumed = true;
					return m_value.get_future().get();
				}

				[[nodiscard]]
				Task<TResult> get_return_object()
				{
					return Task<TResult>{ Task<TResult>::handle_type::from_promise(*this) };
				}

				void unhandled_exception()
				{
					m_value.set_exception(std::current_exception());
					m_isResultSet = true;
				}
			};

			template <>
			class Promise<void> : public PromiseBase
			{
			private:
				std::promise<void> m_value;
				bool m_isResultSet = false;
				bool m_resultConsumed = false;

			public:
				Promise() = default;

				Promise(Promise<void>&&) noexcept = default;

				Promise<void>& operator=(Promise<void>&&) = delete;

				void return_void()
				{
					m_value.set_value();
					m_isResultSet = true;
				}

				void value()
				{
					if (!m_isResultSet)
					{
						throw Error{ U"Task is not completed." };
					}
					if (m_resultConsumed)
					{
						throw Error{ U"Task result can be get only once." };
					}
					m_resultConsumed = true;
					m_value.get_future().get();
				}

				[[nodiscard]]
				Task<void> get_return_object()
				{
					return Task<void>{ Task<void>::handle_type::from_promise(*this) };
				}

				void unhandled_exception()
				{
					m_value.set_exception(std::current_exception());
					m_isResultSet = true;
				}
			};
		}

		inline Task<void> ScopedTaskRunner::waitUntilDone() const&
		{
			while (!done())
			{
				co_await NextFrame();
			}
		}

		inline Task<void> MultiRunner::waitUntilAllDone() const&
		{
			while (!allDone())
			{
				co_await NextFrame();
			}
		}

		inline Task<void> MultiRunner::waitUntilAnyDone() const&
		{
			while (!anyDone())
			{
				co_await NextFrame();
			}
		}

		template <typename TResult = void>
		class [[nodiscard]] TaskFinishSource
		{
			static_assert(!std::is_reference_v<TResult>, "TResult must not be a reference type");
			static_assert(std::is_move_constructible_v<TResult> || std::is_void_v<TResult>, "TResult must be move constructible");
			static_assert(!std::is_const_v<TResult>, "TResult must not have 'const' qualifier");

		private:
			std::unique_ptr<TResult> m_result;
			bool m_resultConsumed = false;

		public:
			TaskFinishSource() = default;

			TaskFinishSource(const TaskFinishSource&) = delete;

			TaskFinishSource& operator=(const TaskFinishSource&) = delete;

			TaskFinishSource(TaskFinishSource&&) noexcept = default;

			TaskFinishSource& operator=(TaskFinishSource&&) = delete;

			~TaskFinishSource() noexcept = default;

			bool requestFinish(const TResult& result) requires std::is_copy_constructible_v<TResult>
			{
				if (m_resultConsumed || hasResult())
				{
					return false;
				}
				m_result = std::make_unique<TResult>(result);
				return true;
			}

			bool requestFinish(TResult&& result)
			{
				if (m_resultConsumed || hasResult())
				{
					return false;
				}
				m_result = std::make_unique<TResult>(std::move(result));
				return true;
			}

			[[nodiscard]]
			bool hasResult() const noexcept
			{
				return m_result != nullptr;
			}

			// hasResult()がtrueを返す場合のみ呼び出し可能。1回だけ取得でき、2回目以降の呼び出しは例外を投げる
			[[nodiscard]]
			TResult result()
			{
				if (m_resultConsumed)
				{
					throw Error{ U"TaskFinishSource: result can be get only once. Make sure to check if hasResult() returns true before calling result()." };
				}
				if (m_result == nullptr)
				{
					throw Error{ U"TaskFinishSource: TaskFinishSource does not have a result. Make sure to check if hasResult() returns true before calling result()." };
				}
				m_resultConsumed = true;

				auto result = std::move(*m_result);
				m_result.reset();
				return result;
			}

			[[nodiscard]]
			Task<TResult> waitForResult()
			{
				while (!hasResult())
				{
					co_await NextFrame();
				}
				m_resultConsumed = true;
				co_return *m_result;
			}

			[[nodiscard]]
			Task<void> waitUntilDone() const
			{
				while (!done())
				{
					co_await NextFrame();
				}
			}

			[[nodiscard]]
			bool done() const noexcept
			{
				return m_result != nullptr || m_resultConsumed;
			}
		};

		template <>
		class [[nodiscard]] TaskFinishSource<void>
		{
		private:
			bool m_finishRequested = false;

		public:
			TaskFinishSource() = default;

			TaskFinishSource(const TaskFinishSource&) = delete;

			TaskFinishSource& operator=(const TaskFinishSource&) = delete;

			TaskFinishSource(TaskFinishSource&&) noexcept = default;

			TaskFinishSource& operator=(TaskFinishSource&&) = delete;

			~TaskFinishSource() noexcept = default;

			bool requestFinish()
			{
				if (m_finishRequested)
				{
					return false;
				}
				m_finishRequested = true;
				return true;
			}

			[[nodiscard]]
			Task<void> waitUntilDone() const
			{
				while (!done())
				{
					co_await NextFrame();
				}
			}

			[[nodiscard]]
			bool done() const noexcept
			{
				return m_finishRequested;
			}
		};

		[[nodiscard]]
		inline Task<void> UpdaterTask(std::function<void()> updateFunc)
		{
			while (true)
			{
				updateFunc();
				co_await NextFrame();
			}
		}

		template <typename TResult>
		[[nodiscard]]
		Task<TResult> UpdaterTask(std::function<void(TaskFinishSource<TResult>&)> updateFunc)
		{
			TaskFinishSource<TResult> taskFinishSource;

			while (true)
			{
				updateFunc(taskFinishSource);
				if (taskFinishSource.hasResult())
				{
					co_return taskFinishSource.result();
				}
				co_await NextFrame();
			}
		}

		template <>
		[[nodiscard]]
		inline Task<void> UpdaterTask(std::function<void(TaskFinishSource<void>&)> updateFunc)
		{
			TaskFinishSource<void> taskFinishSource;

			while (true)
			{
				updateFunc(taskFinishSource);
				if (taskFinishSource.done())
				{
					co_return;
				}
				co_await NextFrame();
			}
		}

		template <typename TResult>
		auto operator co_await(Task<TResult>&& rhs)
		{
			return detail::TaskAwaiter<TResult>{ std::move(rhs) };
		}

		template <typename TResult>
		auto operator co_await(const Task<TResult>& rhs) = delete;

		inline void Init()
		{
			detail::Backend::Init();
		}

		[[nodiscard]]
		inline bool HasActiveDrawer(int32 drawIndex)
		{
			return detail::Backend::HasActiveDrawer(drawIndex);
		}

		// drawIndexMin <= drawIndex <= drawIndexMax であるdrawIndexを持つDrawerが存在するか
		// (drawIndexMaxを含む点に注意)
		[[nodiscard]]
		inline bool HasActiveDrawerInRange(int32 drawIndexMin, int32 drawIndexMax)
		{
			return detail::Backend::HasActiveDrawerInRange(drawIndexMin, drawIndexMax);
		}

		[[nodiscard]]
		inline bool HasActiveModal()
		{
			return detail::Backend::HasActiveDrawerInRange(DrawIndex::ModalMin, DrawIndex::ModalMax);
		}

		[[nodiscard]]
		inline bool HasActiveFadeIn()
		{
			return detail::Backend::HasActiveDrawerInRange(DrawIndex::FadeInMin, DrawIndex::FadeInMax);
		}

		[[nodiscard]]
		inline bool HasActiveFadeOut()
		{
			return detail::Backend::HasActiveDrawerInRange(DrawIndex::FadeOutMin, DrawIndex::FadeOutMax);
		}

		[[nodiscard]]
		inline bool HasActiveFade()
		{
			return HasActiveFadeIn() || HasActiveFadeOut();
		}

		template <typename TResult>
		[[nodiscard]]
		Task<TResult> FromResult(TResult result)
		{
			co_return result;
		}

		[[nodiscard]]
		inline Task<void> DelayFrame(int32 frames)
		{
			for (int32 i = 0; i < frames; ++i)
			{
				co_await NextFrame();
			}
		}

		[[nodiscard]]
		inline Task<void> Delay(const Duration duration, ISteadyClock* pSteadyClock = nullptr)
		{
			const Timer timer{ duration, StartImmediately::Yes, pSteadyClock };
			while (!timer.reachedZero())
			{
				co_await NextFrame();
			}
		}

		[[nodiscard]]
		inline Task<void> WaitForever()
		{
			while (true)
			{
				co_await NextFrame();
			}
		}

		namespace detail
		{
			template <class T>
			concept Predicate = std::invocable<T> && std::same_as<std::invoke_result_t<T>, bool>;
		}

		template <detail::Predicate TPredicate>
		[[nodiscard]]
		inline Task<void> WaitUntil(TPredicate predicate)
		{
			while (!predicate())
			{
				co_await NextFrame();
			}
		}

		template <detail::Predicate TPredicate>
		[[nodiscard]]
		inline Task<void> WaitWhile(TPredicate predicate)
		{
			while (predicate())
			{
				co_await NextFrame();
			}
		}

		template <typename T>
		[[nodiscard]]
		Task<T> WaitForResult(const std::optional<T>* pOptional)
		{
			while (!pOptional->has_value())
			{
				co_await NextFrame();
			}
			co_return **pOptional;
		}

		template <typename T>
		[[nodiscard]]
		Task<T> WaitForResult(const Optional<T>* pOptional)
		{
			while (!pOptional->has_value())
			{
				co_await NextFrame();
			}
			co_return **pOptional;
		}

		template <typename T>
		[[nodiscard]]
		Task<void> WaitUntilHasValue(const std::optional<T>* pOptional)
		{
			while (!pOptional->has_value())
			{
				co_await NextFrame();
			}
		}

		template <typename T>
		[[nodiscard]]
		Task<void> WaitUntilHasValue(const Optional<T>* pOptional)
		{
			while (!pOptional->has_value())
			{
				co_await NextFrame();
			}
		}

		template <typename T>
		[[nodiscard]]
		Task<void> WaitUntilValueChanged(const T* pValue)
		{
			const T initialValue = *pValue;
			while (*pValue == initialValue)
			{
				co_await NextFrame();
			}
		}

		[[nodiscard]]
		inline Task<void> WaitForTimer(const Timer* pTimer)
		{
			while (!pTimer->reachedZero())
			{
				co_await NextFrame();
			}
		}

		template <class TInput>
		[[nodiscard]]
		Task<void> WaitForDown(const TInput input)
		{
			while (!input.down())
			{
				co_await NextFrame();
			}
		}

		template <class TInput>
		[[nodiscard]]
		Task<void> WaitForUp(const TInput input)
		{
			while (!input.up())
			{
				co_await NextFrame();
			}
		}

		template <class TArea>
		[[nodiscard]]
		Task<void> WaitForLeftClicked(const TArea area)
		{
			while (!area.leftClicked())
			{
				co_await NextFrame();
			}
		}

		template <class TArea>
		[[nodiscard]]
		Task<void> WaitForLeftReleased(const TArea area)
		{
			while (!area.leftReleased())
			{
				co_await NextFrame();
			}
		}

		template <class TArea>
		[[nodiscard]]
		Task<void> WaitForLeftClickedThenReleased(const TArea area)
		{
			while (true)
			{
				if (area.leftClicked())
				{
					const auto [releasedInArea, _] = co_await Any(WaitForLeftReleased(area), WaitForUp(MouseL));
					if (releasedInArea.has_value())
					{
						break;
					}
				}
				co_await NextFrame();
			}
		}

		template <class TArea>
		[[nodiscard]]
		Task<void> WaitForRightClicked(const TArea area)
		{
			while (!area.rightClicked())
			{
				co_await NextFrame();
			}
		}

		template <class TArea>
		[[nodiscard]]
		Task<void> WaitForRightReleased(const TArea area)
		{
			while (!area.rightReleased())
			{
				co_await NextFrame();
			}
		}

		template <class TArea>
		[[nodiscard]]
		Task<void> WaitForRightClickedThenReleased(const TArea area)
		{
			while (true)
			{
				if (area.rightClicked())
				{
					const auto [releasedInArea, _] = co_await Any(WaitForRightReleased(area), WaitForUp(MouseR));
					if (releasedInArea.has_value())
					{
						break;
					}
				}
				co_await NextFrame();
			}
		}

		template <class TArea>
		[[nodiscard]]
		Task<void> WaitForMouseOver(const TArea area)
		{
			while (!area.mouseOver())
			{
				co_await NextFrame();
			}
		}

		// voidの参照やvoidを含むタプルは使用できないため、voidの代わりに戻り値として返すための空の構造体を用意
		struct VoidResult
		{
		};

		namespace detail
		{
			template <typename TResult>
			using VoidResultTypeReplace = std::conditional_t<std::is_void_v<TResult>, VoidResult, TResult>;

			template <typename TResult>
			[[nodiscard]]
			auto ConvertVoidResult(const Task<TResult>& task) -> VoidResultTypeReplace<TResult>
			{
				if constexpr (std::is_void_v<TResult>)
				{
					task.value(); // 例外伝搬のためにvoidでも呼び出す
					return VoidResult{};
				}
				else
				{
					return task.value();
				}
			}

			template <typename TResult>
			[[nodiscard]]
			auto ConvertOptionalVoidResult(const Task<TResult>& task) -> Optional<VoidResultTypeReplace<TResult>>
			{
				if (!task.done())
				{
					return none;
				}

				if constexpr (std::is_void_v<TResult>)
				{
					task.value(); // 例外伝搬のためにvoidでも呼び出す
					return MakeOptional(VoidResult{});
				}
				else
				{
					return MakeOptional(task.value());
				}
			}

			template <typename TTask>
			concept TaskConcept = std::is_same_v<TTask, Task<typename TTask::result_type>>;

			template <typename TScene>
			concept SceneConcept = std::derived_from<TScene, SceneBase>;
		}

		template <detail::TaskConcept... TTasks>
		auto All(TTasks... args) -> Task<std::tuple<detail::VoidResultTypeReplace<typename TTasks::result_type>...>>
		{
			if ((args.done() && ...))
			{
				co_return std::make_tuple(detail::ConvertVoidResult(args)...);
			}

			while (true)
			{
				(args.resume(), ...);
				if ((args.done() && ...))
				{
					co_return std::make_tuple(detail::ConvertVoidResult(args)...);
				}
				co_await NextFrame();
			}
		}

		template <detail::TaskConcept... TTasks>
		auto Any(TTasks... args) -> Task<std::tuple<Optional<detail::VoidResultTypeReplace<typename TTasks::result_type>>...>>
		{
			static_assert(
				((std::is_copy_constructible_v<typename TTasks::result_type> || std::is_void_v<typename TTasks::result_type>) && ...),
				"Co::Any does not support tasks that return non-copy-constructible results; use discardResult() if the result is not needed.");

			if ((args.done() || ...))
			{
				co_return std::make_tuple(detail::ConvertOptionalVoidResult(args)...);
			}

			while (true)
			{
				(args.resume(), ...);
				if ((args.done() || ...))
				{
					co_return std::make_tuple(detail::ConvertOptionalVoidResult(args)...);
				}
				co_await NextFrame();
			}
		}

		template <typename TFunc, typename... Args>
		[[nodiscard]]
		auto AsyncThread(TFunc func, Args... args) -> Task<std::invoke_result_t<TFunc, Args...>>
			requires std::invocable<TFunc, Args...>
		{
			auto future = std::async(std::launch::async, std::move(func), std::move(args)...);
			co_await WaitUntil([&future] { return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
			co_return future.get();
		}
	}
}

#ifndef NO_COTASKLIB_USING
using namespace cotasklib;
#endif
