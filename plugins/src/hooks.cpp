#include "hooks.h"
#include "main.h"
#include "func_pool.h"

#include "sdk/amx/amx.h"
#include "sdk/plugincommon.h"
#include "subhook/subhook.h"

#include <cassert>
#include <vector>
#include <unordered_map>

extern void *pAMXFunctions;

template <class FType>
class amx_hook_func;

template <class Ret, class... Args>
class amx_hook_func<Ret(*)(Args...)>
{
public:
	typedef Ret hook_ftype(Ret(*)(Args...), Args...);

	typedef Ret AMXAPI handler_ftype(Args...);

	template <subhook_t &Hook, hook_ftype *Handler>
	static Ret AMXAPI handler(Args... args)
	{
		return Handler(reinterpret_cast<Ret(*)(Args...)>(subhook_get_trampoline(Hook)), args...);
	}
};

template <int Index>
class amx_hook
{
	static subhook_t hook;

public:
	template <class FType, typename amx_hook_func<FType>::hook_ftype *Func>
	struct ctl
	{
		static void load()
		{
			typename amx_hook_func<FType>::handler_ftype *hookfn = &amx_hook_func<FType>::template handler<hook, Func>;

			hook = subhook_new(reinterpret_cast<void*>(((FType*)pAMXFunctions)[Index]), reinterpret_cast<void*>(hookfn), {});
			subhook_install(hook);
		}

		static void unload()
		{
			subhook_remove(hook);
			subhook_free(hook);
		}

		static FType orig()
		{
			if(subhook_is_installed(hook))
			{
				return reinterpret_cast<FType>(subhook_get_trampoline(hook));
			}else{
				return ((FType*)pAMXFunctions)[Index];
			}
		}
	};
};

template <int index>
subhook_t amx_hook<index>::hook;

#define AMX_HOOK_FUNC(Func, ...) Func(decltype(&::Func) orig, __VA_ARGS__)
namespace hooks
{
	constexpr size_t fallback_pool_size = 256;

	std::vector<std::string> functions;
	std::unordered_map<std::string, AMX_NATIVE> registered;

	cell fallback_handler(size_t id, AMX *amx, cell *params)
	{
		logprintf("[NativeFallback] Implementation for '%s' was not provided.", functions[id].c_str());
		amx_RaiseError(amx, AMX_ERR_NOTFOUND);
		return 0;
	}

	cell default_fallback_handler(AMX *amx, cell *params)
	{
		logprintf("[NativeFallback] Implementation for native function was not provided.");
		amx_RaiseError(amx, AMX_ERR_NOTFOUND);
		return 0;
	}

	using pool = aux::func<cell AMX_NATIVE_CALL(AMX *amx, cell *params)>::pool<fallback_pool_size, fallback_handler>;

	int AMX_HOOK_FUNC(amx_Exec, AMX *amx, cell *retval, int index)
	{
		if(amx && (amx->flags & AMX_FLAG_NTVREG) == 0)
		{
			int num;
			amx_NumNatives(amx, &num);

			std::vector<AMX_NATIVE_INFO> fallback;
			auto hdr = reinterpret_cast<AMX_HEADER*>(amx->base);
			for(int i = 0; i < num; i++)
			{
				auto native = reinterpret_cast<cell*>(amx->base + hdr->natives + i * hdr->defsize);
				if(*native == 0)
				{
					const char *name;
					if(hdr->defsize == sizeof(AMX_FUNCSTUBNT))
					{
						name = reinterpret_cast<char*>(amx->base + reinterpret_cast<AMX_FUNCSTUBNT*>(native)->nameofs);
					}else{
						name = reinterpret_cast<AMX_FUNCSTUB*>(native)->name;
					}

					logprintf("[NativeFallback] Native function '%s' was not registered.", name);

					auto it = registered.find(name);
					if(it != registered.end())
					{
						fallback.emplace_back(AMX_NATIVE_INFO{name, it->second});
					}else{
						auto next = pool::add();
						if(next.first == -1)
						{
							logprintf("[NativeFallback] Fallback function pool is full! Increase fallback_pool_size and recompile (currently %d).", fallback_pool_size);
							next.first = functions.size();
							next.second = default_fallback_handler;
						}

						assert(next.first == functions.size());
						functions.emplace_back(name);

						fallback.emplace_back(AMX_NATIVE_INFO{name, next.second});
						registered[name] = next.second;
					}
				}
			}

			amx_Register(amx, fallback.data(), fallback.size());
		}
		return orig(amx, retval, index);
	}
}

#define amx_Hook(Func) amx_hook<PLUGIN_AMX_EXPORT_##Func>::ctl<decltype(&::amx_##Func), &hooks::amx_##Func>

void hooks::load()
{
	amx_Hook(Exec)::load();
}

void hooks::unload()
{
	amx_Hook(Exec)::unload();
}
