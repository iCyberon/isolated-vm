#include <node.h>
#include "util.h"
#include "shareable_isolate.h"
#include "isolate_handle.h"
#include "context_handle.h"
#include "native_module_handle.h"
#include "script_handle.h"
#include "external_copy_handle.h"
#include "lib_handle.h"
#include "reference_handle.h"
#include "platform_delegate.h"

#include <memory>

using namespace std;

namespace ivm {

/**
 * The whole library is transferable so you can Inception the library into your isolates.
 */
class LibraryHandle : public TransferableHandle {
	private:
		class LibraryHandleTransferable : public Transferable {
			public:
				LibraryHandleTransferable() {}
				virtual Local<Value> TransferIn() {
					return LibraryHandle::Get();
				}
		};

	public:
		LibraryHandle() {}

		static ShareableIsolate::IsolateSpecific<FunctionTemplate>& TemplateSpecific() {
			static ShareableIsolate::IsolateSpecific<FunctionTemplate> tmpl;
			return tmpl;
		}

		static Local<FunctionTemplate> Definition() {
			auto tmpl = Inherit<TransferableHandle>(MakeClass("isolated_vm", nullptr, 0));
			AddProtoTemplate(tmpl, "Context", ClassHandle::GetFunctionTemplate<ContextHandle>());
			AddProtoTemplate(tmpl, "ExternalCopy", ClassHandle::GetFunctionTemplate<ExternalCopyHandle>());
			AddProtoTemplate(tmpl, "Isolate", ClassHandle::GetFunctionTemplate<IsolateHandle>());
			AddProtoTemplate(tmpl, "NativeModule", ClassHandle::GetFunctionTemplate<NativeModuleHandle>());
			AddProtoTemplate(tmpl, "Reference", ClassHandle::GetFunctionTemplate<ReferenceHandle>());
			AddProtoTemplate(tmpl, "Script", ClassHandle::GetFunctionTemplate<ScriptHandle>());
			return tmpl;
		}

		virtual unique_ptr<Transferable> TransferOut() {
			return std::make_unique<LibraryHandleTransferable>();
		}

		static Local<Object> Get() {
			Local<Object> library = ClassHandle::NewInstance<LibraryHandle>().As<Object>();
			library->Set(v8_symbol("lib"), ClassHandle::NewInstance<LibHandle>());
			return library;
		}
};

// Module entry point
shared_ptr<ShareableIsolate> root_isolate;
extern "C"
void init(Local<Object> target) {

	// These flags will override limits set through code. Since the main node isolate is already
	// created we can reset these so they won't affect the isolates we make.
	int argc = 2;
	const char* flags[] = {
		"--max-semi-space-size", "0",
		"--max-old-space-size", "0"
	};
	V8::SetFlagsFromCommandLine(&argc, (char**)flags, false);

	Isolate* isolate = Isolate::GetCurrent();
	root_isolate = make_shared<ShareableIsolate>(isolate, isolate->GetCurrentContext());
	target->Set(v8_symbol("ivm"), LibraryHandle::Get());
	PlatformDelegate::InitializeDelegate();
}

}

NODE_MODULE(isolated_vm, ivm::init)
