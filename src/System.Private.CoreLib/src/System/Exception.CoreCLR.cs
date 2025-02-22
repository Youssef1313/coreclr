// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Collections;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;

namespace System
{
    public partial class Exception : ISerializable
    {
        partial void RestoreRemoteStackTrace(SerializationInfo info, StreamingContext context)
        {
            _remoteStackTraceString = info.GetString("RemoteStackTraceString"); // Do not rename (binary serialization)

            // Get the WatsonBuckets that were serialized - this is particularly
            // done to support exceptions going across AD transitions.
            //
            // We use the no throw version since we could be deserializing a pre-V4
            // exception object that may not have this entry. In such a case, we would
            // get null.
            _watsonBuckets = info.GetValueNoThrow("WatsonBuckets", typeof(byte[])); // Do not rename (binary serialization)

            // If we are constructing a new exception after a cross-appdomain call...
            if (context.State == StreamingContextStates.CrossAppDomain)
            {
                // ...this new exception may get thrown.  It is logically a re-throw, but
                //  physically a brand-new exception.  Since the stack trace is cleared
                //  on a new exception, the "_remoteStackTraceString" is provided to
                //  effectively import a stack trace from a "remote" exception.  So,
                //  move the _stackTraceString into the _remoteStackTraceString.  Note
                //  that if there is an existing _remoteStackTraceString, it will be
                //  preserved at the head of the new string, so everything works as
                //  expected.
                // Even if this exception is NOT thrown, things will still work as expected
                //  because the StackTrace property returns the concatenation of the
                //  _remoteStackTraceString and the _stackTraceString.
                _remoteStackTraceString += _stackTraceString;
                _stackTraceString = null;
            }
        }

        private IDictionary CreateDataContainer()
        {
            if (IsImmutableAgileException(this))
                return new EmptyReadOnlyDictionaryInternal();
            else
                return new ListDictionaryInternal();
        }

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern bool IsImmutableAgileException(Exception e);

#if FEATURE_COMINTEROP
        //
        // Exception requires anything to be added into Data dictionary is serializable
        // This wrapper is made serializable to satisfy this requirement but does NOT serialize
        // the object and simply ignores it during serialization, because we only need
        // the exception instance in the app to hold the error object alive.
        // Once the exception is serialized to debugger, debugger only needs the error reference string
        //
        [Serializable]
        internal class __RestrictedErrorObject
        {
            // Hold the error object instance but don't serialize/deserialize it
            [NonSerialized]
            private readonly object _realErrorObject;

            internal __RestrictedErrorObject(object errorObject)
            {
                _realErrorObject = errorObject;
            }

            public object RealErrorObject
            {
                get
                {
                    return _realErrorObject;
                }
            }
        }

        internal void AddExceptionDataForRestrictedErrorInfo(
            string restrictedError,
            string restrictedErrorReference,
            string restrictedCapabilitySid,
            object? restrictedErrorObject,
            bool hasrestrictedLanguageErrorObject = false)
        {
            IDictionary dict = Data;
            if (dict != null)
            {
                dict.Add("RestrictedDescription", restrictedError);
                dict.Add("RestrictedErrorReference", restrictedErrorReference);
                dict.Add("RestrictedCapabilitySid", restrictedCapabilitySid);

                // Keep the error object alive so that user could retrieve error information
                // using Data["RestrictedErrorReference"]
                dict.Add("__RestrictedErrorObject", restrictedErrorObject == null ? null : new __RestrictedErrorObject(restrictedErrorObject));
                dict.Add("__HasRestrictedLanguageErrorObject", hasrestrictedLanguageErrorObject);
            }
        }

        internal bool TryGetRestrictedLanguageErrorObject(out object? restrictedErrorObject)
        {
            restrictedErrorObject = null;
            if (Data != null && Data.Contains("__HasRestrictedLanguageErrorObject"))
            {
                if (Data.Contains("__RestrictedErrorObject"))
                {
                    if (Data["__RestrictedErrorObject"] is __RestrictedErrorObject restrictedObject)
                        restrictedErrorObject = restrictedObject.RealErrorObject;
                }
                return (bool)Data["__HasRestrictedLanguageErrorObject"]!;
            }

            return false;
        }
#endif // FEATURE_COMINTEROP

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern IRuntimeMethodInfo GetMethodFromStackTrace(object stackTrace);

        private MethodBase? GetExceptionMethodFromStackTrace()
        {
            Debug.Assert(_stackTrace != null, "_stackTrace shouldn't be null when this method is called");
            IRuntimeMethodInfo method = GetMethodFromStackTrace(_stackTrace!);

            // Under certain race conditions when exceptions are re-used, this can be null
            if (method == null)
                return null;

            return RuntimeType.GetMethodBase(method);
        }

        public MethodBase? TargetSite
        {
            get
            {
                if (_exceptionMethod != null)
                {
                    return _exceptionMethod;
                }
                if (_stackTrace == null)
                {
                    return null;
                }

                _exceptionMethod = GetExceptionMethodFromStackTrace();
                return _exceptionMethod;
            }
        }

        // Returns the stack trace as a string.  If no stack trace is
        // available, null is returned.
        public virtual string? StackTrace
        {
            get
            {
                string? stackTraceString = _stackTraceString;
                string? remoteStackTraceString = _remoteStackTraceString;

                // if no stack trace, try to get one
                if (stackTraceString != null)
                {
                    return remoteStackTraceString + stackTraceString;
                }
                if (_stackTrace == null)
                {
                    return remoteStackTraceString;
                }

                return remoteStackTraceString + GetStackTrace(this);
            }
        }

        private static string GetStackTrace(Exception e)
        {
            // Do not include a trailing newline for backwards compatibility
            return new StackTrace(e, fNeedFileInfo: true).ToString(System.Diagnostics.StackTrace.TraceFormat.Normal);
        }

        private string? CreateSourceName()
        {
            StackTrace st = new StackTrace(this, fNeedFileInfo: false);
            if (st.FrameCount > 0)
            {
                StackFrame sf = st.GetFrame(0)!;
                MethodBase method = sf.GetMethod()!;

                Module module = method.Module;

                if (!(module is RuntimeModule rtModule))
                {
                    if (module is System.Reflection.Emit.ModuleBuilder moduleBuilder)
                        rtModule = moduleBuilder.InternalModule;
                    else
                        throw new ArgumentException(SR.Argument_MustBeRuntimeReflectionObject);
                }

                return rtModule.GetRuntimeAssembly().GetSimpleName();
            }

            return null;
        }

        // This method will clear the _stackTrace of the exception object upon deserialization
        // to ensure that references from another AD/Process dont get accidentally used.
        [OnDeserialized]
        private void OnDeserialized(StreamingContext context)
        {
            _stackTrace = null;

            // We wont serialize or deserialize the IP for Watson bucketing since
            // we dont know where the deserialized object will be used in.
            // Using it across process or an AppDomain could be invalid and result
            // in AV in the runtime.
            //
            // Hence, we set it to zero when deserialization takes place.
            _ipForWatsonBuckets = UIntPtr.Zero;
        }

        // This is used by the runtime when re-throwing a managed exception.  It will
        //  copy the stack trace to _remoteStackTraceString.
        internal void InternalPreserveStackTrace()
        {
            // Make sure that the _source field is initialized if Source is not overriden.
            // We want it to contain the original faulting point.
            _ = Source;

            string? tmpStackTraceString = StackTrace;

            if (!string.IsNullOrEmpty(tmpStackTraceString))
            {
                _remoteStackTraceString = tmpStackTraceString + Environment.NewLine;
            }

            _stackTrace = null;
            _stackTraceString = null;
        }

        // This is the object against which a lock will be taken
        // when attempt to restore the EDI. Since its static, its possible
        // that unrelated exception object restorations could get blocked
        // for a small duration but that sounds reasonable considering
        // such scenarios are going to be extremely rare, where timing
        // matches precisely.
        private static readonly object s_DispatchStateLock = new object();

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern void PrepareForForeignExceptionRaise();

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern void GetStackTracesDeepCopy(Exception exception, out object currentStackTrace, out object dynamicMethodArray);

        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern void SaveStackTracesFromDeepCopy(Exception exception, object? currentStackTrace, object? dynamicMethodArray);

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern object CopyStackTrace(object currentStackTrace);

        [MethodImpl(MethodImplOptions.InternalCall)]
        private static extern object CopyDynamicMethods(object currentDynamicMethods);

        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern uint GetExceptionCount();

        internal static object? DeepCopyStackTrace(object? currentStackTrace)
        {
            if (currentStackTrace != null)
            {
                return CopyStackTrace(currentStackTrace);
            }
            else
            {
                return null;
            }
        }

        internal static object? DeepCopyDynamicMethods(object? currentDynamicMethods)
        {
            if (currentDynamicMethods != null)
            {
                return CopyDynamicMethods(currentDynamicMethods);
            }
            else
            {
                return null;
            }
        }

        // This is invoked by ExceptionDispatchInfo.Throw to restore the exception stack trace, corresponding to the original throw of the
        // exception, just before the exception is "rethrown".
        internal void RestoreDispatchState(in DispatchState dispatchState)
        {
            bool fCanProcessException = !IsImmutableAgileException(this);
            // Restore only for non-preallocated exceptions
            if (fCanProcessException)
            {
                // Take a lock to ensure only one thread can restore the details
                // at a time against this exception object that could have
                // multiple ExceptionDispatchInfo instances associated with it.
                //
                // We do this inside a finally clause to ensure ThreadAbort cannot
                // be injected while we have taken the lock. This is to prevent
                // unrelated exception restorations from getting blocked due to TAE.
                try { }
                finally
                {
                    // When restoring back the fields, we again create a copy and set reference to them
                    // in the exception object. This will ensure that when this exception is thrown and these
                    // fields are modified, then EDI's references remain intact.
                    //
                    // Since deep copying can throw on OOM, try to get the copies
                    // outside the lock.
                    object? _stackTraceCopy = (dispatchState.StackTrace == null) ? null : DeepCopyStackTrace(dispatchState.StackTrace);
                    object? _dynamicMethodsCopy = (dispatchState.DynamicMethods == null) ? null : DeepCopyDynamicMethods(dispatchState.DynamicMethods);

                    // Finally, restore the information.
                    //
                    // Since EDI can be created at various points during exception dispatch (e.g. at various frames on the stack) for the same exception instance,
                    // they can have different data to be restored. Thus, to ensure atomicity of restoration from each EDI, perform the restore under a lock.
                    lock (s_DispatchStateLock)
                    {
                        _watsonBuckets = dispatchState.WatsonBuckets;
                        _ipForWatsonBuckets = dispatchState.IpForWatsonBuckets;
                        _remoteStackTraceString = dispatchState.RemoteStackTrace;
                        SaveStackTracesFromDeepCopy(this, _stackTraceCopy, _dynamicMethodsCopy);
                    }
                    _stackTraceString = null;

                    // Marks the TES state to indicate we have restored foreign exception
                    // dispatch information.
                    PrepareForForeignExceptionRaise();
                }
            }
        }

        private MethodBase? _exceptionMethod;  // Needed for serialization.
        internal string? _message;
        private IDictionary? _data;
        private readonly Exception? _innerException;
        private string? _helpURL;
        private object? _stackTrace;
        private object? _watsonBuckets;
        private string? _stackTraceString; // Needed for serialization.
        private string? _remoteStackTraceString;
#pragma warning disable CA1823, 414  // Fields are not used from managed.
        // _dynamicMethods is an array of System.Resolver objects, used to keep
        // DynamicMethodDescs alive for the lifetime of the exception. We do this because
        // the _stackTrace field holds MethodDescs, and a DynamicMethodDesc can be destroyed
        // unless a System.Resolver object roots it.
        private readonly object? _dynamicMethods;
        private string? _source;         // Mainly used by VB.
        private UIntPtr _ipForWatsonBuckets; // Used to persist the IP for Watson Bucketing
        private readonly IntPtr _xptrs;             // Internal EE stuff
        private readonly int _xcode = _COMPlusExceptionCode;             // Internal EE stuff
#pragma warning restore CA1823, 414

        // @MANAGED: HResult is used from within the EE!  Rename with care - check VM directory
        private int _HResult;       // HResult

        // See src\inc\corexcep.h's EXCEPTION_COMPLUS definition:
        private const int _COMPlusExceptionCode = unchecked((int)0xe0434352);   // Win32 exception code for COM+ exceptions

        private string? SerializationRemoteStackTraceString => _remoteStackTraceString;

        private object? SerializationWatsonBuckets => _watsonBuckets;

        private string? SerializationStackTraceString
        {
            get
            {
                string? stackTraceString = _stackTraceString;

                if (stackTraceString == null && _stackTrace != null)
                {
                    stackTraceString = GetStackTrace(this);
                }

                return stackTraceString;
            }
        }

        // This piece of infrastructure exists to help avoid deadlocks
        // between parts of mscorlib that might throw an exception while
        // holding a lock that are also used by mscorlib's ResourceManager
        // instance.  As a special case of code that may throw while holding
        // a lock, we also need to fix our asynchronous exceptions to use
        // Win32 resources as well (assuming we ever call a managed
        // constructor on instances of them).  We should grow this set of
        // exception messages as we discover problems, then move the resources
        // involved to native code.
        internal enum ExceptionMessageKind
        {
            ThreadAbort = 1,
            ThreadInterrupted = 2,
            OutOfMemory = 3
        }

        // See comment on ExceptionMessageKind
        internal static string GetMessageFromNativeResources(ExceptionMessageKind kind)
        {
            string? retMesg = null;
            GetMessageFromNativeResources(kind, JitHelpers.GetStringHandleOnStack(ref retMesg));
            return retMesg!;
        }

        [DllImport(JitHelpers.QCall, CharSet = CharSet.Unicode)]
        private static extern void GetMessageFromNativeResources(ExceptionMessageKind kind, StringHandleOnStack retMesg);

        internal readonly struct DispatchState
        {
            public readonly object? StackTrace;
            public readonly object? DynamicMethods;
            public readonly string? RemoteStackTrace;
            public readonly UIntPtr IpForWatsonBuckets;
            public readonly object? WatsonBuckets;

            public DispatchState(
                object? stackTrace,
                object? dynamicMethods,
                string? remoteStackTrace,
                UIntPtr ipForWatsonBuckets,
                object? watsonBuckets)
            {
                StackTrace = stackTrace;
                DynamicMethods = dynamicMethods;
                RemoteStackTrace = remoteStackTrace;
                IpForWatsonBuckets = ipForWatsonBuckets;
                WatsonBuckets = watsonBuckets;
            }
        }

        internal DispatchState CaptureDispatchState()
        {
            GetStackTracesDeepCopy(this, out object? stackTrace, out object? dynamicMethods);

            return new DispatchState(stackTrace, dynamicMethods,
                _remoteStackTraceString, _ipForWatsonBuckets, _watsonBuckets);
        }
    }
}
