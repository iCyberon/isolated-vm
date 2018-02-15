#pragma once
#include <v8.h>
#include <cassert>
#include <string>
#include <functional>

namespace ivm {

template <typename T> v8::Local<T> Unmaybe(v8::MaybeLocal<T> handle);
template <typename T> T Unmaybe(v8::Maybe<T> handle);

/**
 * Easy strings
 */
inline v8::Local<v8::String> v8_string(const char* string) {
	return Unmaybe(v8::String::NewFromOneByte(v8::Isolate::GetCurrent(), (const uint8_t*)string, v8::NewStringType::kNormal)); // NOLINT
}

inline v8::Local<v8::String> v8_symbol(const char* string) {
	return Unmaybe(v8::String::NewFromOneByte(v8::Isolate::GetCurrent(), (const uint8_t*)string, v8::NewStringType::kInternalized)); // NOLINT
}

/**
 * Option checker
 */
inline bool IsOptionSet(const v8::Local<v8::Context>& context, const v8::Local<v8::Object>& options, const char* key) {
	return Unmaybe(Unmaybe(options->Get(context, v8_string(key)))->ToBoolean(context))->IsTrue();
}

/**
 * JS + C++ exception, use with care
 */
class js_runtime_error : public std::exception {};
class js_fatal_error : public std::exception {};

template <v8::Local<v8::Value> (*F)(v8::Local<v8::String>)>
struct js_error : public js_runtime_error {
	explicit js_error(const std::string& message) {
		v8::Isolate* isolate = v8::Isolate::GetCurrent();
		const uint8_t* c_str = (const uint8_t*)message.c_str(); // NOLINT
		v8::MaybeLocal<v8::String> maybe_message = v8::String::NewFromOneByte(isolate, c_str, v8::NewStringType::kNormal);
		v8::Local<v8::String> message_handle;
		if (maybe_message.ToLocal(&message_handle)) {
			isolate->ThrowException(F(message_handle));
		}
		// If the MaybeLocal is empty then I think v8 will have an exception on deck. I don't know if
		// there's any way to assert() this though.
	}
};

using js_generic_error = js_error<v8::Exception::Error>;
using js_type_error = js_error<v8::Exception::TypeError>;
using js_range_error = js_error<v8::Exception::RangeError>;

/**
 * Convert a MaybeLocal<T> to Local<T> and throw an error if it's empty. Someone else should throw
 * the v8 exception.
 */
template <typename T>
v8::Local<T> Unmaybe(v8::MaybeLocal<T> handle) {
	v8::Local<T> local;
	if (handle.ToLocal(&local)) {
		return local;
	} else {
		throw js_runtime_error();
	}
}

template <typename T>
T Unmaybe(v8::Maybe<T> handle) {
	T just;
	if (handle.To(&just)) {
		return just;
	} else {
		throw js_runtime_error();
	}
}

/**
 * Shorthand dereference of Persistent to Local
 */
template <typename T>
v8::Local<T> Deref(const v8::Persistent<T>& handle) {
	return v8::Local<T>::New(v8::Isolate::GetCurrent(), handle);
}

/**
 * Sets `stack` accessor on this error object which renders the given stack
 */
v8::Local<v8::Value> AttachStack(v8::Local<v8::Value> error, v8::Local<v8::StackTrace> stack);

/**
 * Run a function and annotate the exception with source / line number if it throws
 */
// TODO: This is only used by isolate_handle.h -- move this to .cc file
template <typename T, typename F>
T RunWithAnnotatedErrors(F&& fn) {
	v8::Isolate* isolate = v8::Isolate::GetCurrent();
	v8::TryCatch try_catch(isolate);
	try {
		return fn();
	} catch (const js_runtime_error& cc_error) {
		try {
			assert(try_catch.HasCaught());
			v8::Local<v8::Context> context = isolate->GetCurrentContext();
			v8::Local<v8::Value> error = try_catch.Exception();
			v8::Local<v8::Message> message = try_catch.Message();
			assert(error->IsObject());
			int linenum = Unmaybe(message->GetLineNumber(context));
			int start_column = Unmaybe(message->GetStartColumn(context));
			std::string decorator =
				std::string(*v8::String::Utf8Value(message->GetScriptResourceName())) +
				":" + std::to_string(linenum) +
				":" + std::to_string(start_column + 1);
			std::string message_str = *v8::String::Utf8Value(Unmaybe(error.As<v8::Object>()->Get(context, v8_symbol("message"))));
			Unmaybe(error.As<v8::Object>()->Set(context, v8_symbol("message"), v8_string((message_str + " [" + decorator + "]").c_str())));
			isolate->ThrowException(error);
			throw js_runtime_error();
		} catch (const js_runtime_error& cc_error) {
			try_catch.ReThrow();
			throw js_runtime_error();
		}
	}
}

} // namespace ivm
