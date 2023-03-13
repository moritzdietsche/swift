//===--- RuntimeValueWitness.cpp - Value Witness Runtime Implementation---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Implementations of runtime determined value witness functions
// This file is intended to be statically linked into executables until it is
// fully added to the runtime.
//
//===----------------------------------------------------------------------===//

#include "BytecodeLayouts.h"
#include "../../public/runtime/WeakReference.h"
#include "../../public/SwiftShims/swift/shims/HeapObject.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/ABI/System.h"
#include "swift/Runtime/Error.h"
#include "swift/Runtime/HeapObject.h"
#include "llvm/Support/SwapByteOrder.h"
#include <cstdint>
#if SWIFT_OBJC_INTEROP
#include "swift/Runtime/ObjCBridge.h"
#include <Block.h>
#endif
#if SWIFT_PTRAUTH
#include <ptrauth.h>
#endif

using namespace swift;

static const size_t layoutStringHeaderSize = sizeof(size_t);

/// Given a pointer and an offset, read the requested data and increment the
/// offset
template <typename T>
T readBytes(const uint8_t *typeLayout, size_t &i) {
  T returnVal = *(const T *)(typeLayout + i);
  i += sizeof(T);
  return returnVal;
}

/// Given a pointer, a value, and an offset, write the value at the given
/// offset in big-endian order
template <typename T>
void writeBytes(uint8_t *typeLayout, size_t i, T value) {
  *((T*)(typeLayout + i)) = value;
}

Metadata *getExistentialTypeMetadata(OpaqueValue *object) {
  return reinterpret_cast<Metadata**>(object)[NumWords_ValueBuffer];
}

typedef Metadata* (*MetadataAccessor)(const Metadata* const *);

const Metadata *getResilientTypeMetadata(const Metadata* metadata, const uint8_t *layoutStr, size_t &offset) {
  auto fnPtr = readBytes<uintptr_t>(layoutStr, offset);
  MetadataAccessor fn;

#if SWIFT_PTRAUTH
  fn = (MetadataAccessor)ptrauth_sign_unauthenticated(
      (void *)(fnPtr),
      ptrauth_key_function_pointer, 0);
#else
  fn = (MetadataAccessor)(fnPtr);
#endif

  return fn(metadata->getGenericArgs());
}

typedef void (*DestrFn)(void*);

struct DestroyFuncAndMask {
  DestrFn fn;
  uintptr_t mask;
  bool isIndirect;
};

void skipDestroy(void* ignore) { }

void existential_destroy(OpaqueValue* object) {
  auto* metadata = getExistentialTypeMetadata(object);
  if (metadata->getValueWitnesses()->isValueInline()) {
    metadata->vw_destroy(object);
  } else {
    swift_release(*(HeapObject**)object);
  }
}

const DestroyFuncAndMask destroyTable[] = {
  {(DestrFn)&skipDestroy, UINTPTR_MAX, false},
  {(DestrFn)&swift_errorRelease, UINTPTR_MAX, true},
  {(DestrFn)&swift_release, ~heap_object_abi::SwiftSpareBitsMask, true},
  {(DestrFn)&swift_unownedRelease, ~heap_object_abi::SwiftSpareBitsMask, true},
  {(DestrFn)&swift_weakDestroy, UINTPTR_MAX, false},
  {(DestrFn)&swift_unknownObjectRelease, ~heap_object_abi::SwiftSpareBitsMask, true},
  {(DestrFn)&swift_unknownObjectUnownedDestroy, UINTPTR_MAX, false},
  {(DestrFn)&swift_unknownObjectWeakDestroy, UINTPTR_MAX, false},
  {(DestrFn)&swift_bridgeObjectRelease, ~heap_object_abi::SwiftSpareBitsMask, true},
#if SWIFT_OBJC_INTEROP
  {(DestrFn)&_Block_release, UINTPTR_MAX, true},
  {(DestrFn)&swift_unknownObjectRelease, UINTPTR_MAX, true},
#else
  {nullptr, UINTPTR_MAX, true},
  {nullptr, UINTPTR_MAX, true},
#endif
  // TODO: how to handle Custom?
  {nullptr, UINTPTR_MAX, true},
  {nullptr, UINTPTR_MAX, true},
  {nullptr, UINTPTR_MAX, true},
  {(DestrFn)&existential_destroy, UINTPTR_MAX, false},
};

extern "C" void
swift_generic_destroy(swift::OpaqueValue *address, const Metadata *metadata) {
  uint8_t *addr = (uint8_t *)address;

  const uint8_t *typeLayout = metadata->getLayoutString();

  size_t offset = layoutStringHeaderSize;
  uintptr_t addrOffset = 0;

  while (true) {
    uint64_t skip = readBytes<uint64_t>(typeLayout, offset);
    auto tag = static_cast<RefCountingKind>(skip >> 56);
    skip &= ~(0xffULL << 56);
    addrOffset += skip;

    if (SWIFT_UNLIKELY(tag == RefCountingKind::End)) {
      return;
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Metatype)) {
      auto typePtr = readBytes<uintptr_t>(typeLayout, offset);
      auto *type = reinterpret_cast<Metadata*>(typePtr);
      type->vw_destroy((OpaqueValue *)(addr + addrOffset));
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Resilient)) {
      auto *type = getResilientTypeMetadata(metadata, typeLayout, offset);
      type->vw_destroy((OpaqueValue *)(addr + addrOffset));
    } else {
      const auto &destroyFunc = destroyTable[static_cast<uint8_t>(tag)];
      if (SWIFT_LIKELY(destroyFunc.isIndirect)) {
        destroyFunc.fn(
            (void *)((*(uintptr_t *)(addr + addrOffset))));
      } else {
        destroyFunc.fn(((void *)(addr + addrOffset)));
      }
    }
  }
}

struct RetainFuncAndMask {
  void* fn;
  uintptr_t mask;
  bool isSingle;
};

#if SWIFT_OBJC_INTEROP
void* Block_copyForwarder(void** dest, const void** src) {
  *dest = _Block_copy(*src);
  return *dest;
}
#endif

typedef void* (*RetainFn)(void*);
typedef void* (*CopyInitFn)(void*, void*);

void* skipRetain(void* ignore) { return nullptr; }
void* existential_initializeWithCopy(OpaqueValue* dest, OpaqueValue* src) {
  auto* metadata = getExistentialTypeMetadata(src);
  return metadata->vw_initializeBufferWithCopyOfBuffer((ValueBuffer*)dest, (ValueBuffer*)src);
}

const RetainFuncAndMask retainTable[] = {
  {(void*)&skipRetain, UINTPTR_MAX, true},
  {(void*)&swift_errorRetain, UINTPTR_MAX, true},
  {(void*)&swift_retain, ~heap_object_abi::SwiftSpareBitsMask, true},
  {(void*)&swift_unownedRetain, ~heap_object_abi::SwiftSpareBitsMask, true},
  {(void*)&swift_weakCopyInit, UINTPTR_MAX, false},
  {(void*)&swift_unknownObjectRetain, ~heap_object_abi::SwiftSpareBitsMask, true},
  {(void*)&swift_unknownObjectUnownedCopyInit, UINTPTR_MAX, false},
  {(void*)&swift_unknownObjectWeakCopyInit, UINTPTR_MAX, false},
  {(void*)&swift_bridgeObjectRetain, ~heap_object_abi::SwiftSpareBitsMask, true},
#if SWIFT_OBJC_INTEROP
  {(void*)&Block_copyForwarder, UINTPTR_MAX, false},
  {(void*)&objc_retain, UINTPTR_MAX, true},
#else
  {nullptr, UINTPTR_MAX, true},
  {nullptr, UINTPTR_MAX, true},
#endif
  // TODO: how to handle Custom?
  {nullptr, UINTPTR_MAX, true},
  {nullptr, UINTPTR_MAX, true},
  {nullptr, UINTPTR_MAX, true},
  {(void*)&existential_initializeWithCopy, UINTPTR_MAX, false},
};

extern "C" swift::OpaqueValue *
swift_generic_initWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata) {
  uintptr_t addrOffset = 0;
  const uint8_t *typeLayout = metadata->getLayoutString();

  size_t size = metadata->vw_size();

  auto offset = layoutStringHeaderSize;

  memcpy(dest, src, size);

  while (true) {
    uint64_t skip = readBytes<uint64_t>(typeLayout, offset);
    auto tag = static_cast<RefCountingKind>(skip >> 56);
    skip &= ~(0xffULL << 56);
    addrOffset += skip;

    if (SWIFT_UNLIKELY(tag == RefCountingKind::End)) {
      return dest;
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Metatype)) {
      auto typePtr = readBytes<uintptr_t>(typeLayout, offset);
      auto *type = reinterpret_cast<Metadata*>(typePtr);
      type->vw_initializeWithCopy((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                  (OpaqueValue*)((uintptr_t)src + addrOffset));
    } else if (SWIFT_UNLIKELY(tag == RefCountingKind::Resilient)) {
      auto *type = getResilientTypeMetadata(metadata, typeLayout, offset);
      type->vw_initializeWithCopy((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                  (OpaqueValue*)((uintptr_t)src + addrOffset));
    } else {
      const auto &retainFunc = retainTable[static_cast<uint8_t>(tag)];
      if (SWIFT_LIKELY(retainFunc.isSingle)) {
        ((RetainFn)retainFunc.fn)(*(void**)(((uintptr_t)dest + addrOffset)));
      } else {
        ((CopyInitFn)retainFunc.fn)((void*)((uintptr_t)dest + addrOffset), (void*)((uintptr_t)src + addrOffset));
      }
    }
  }
}

extern "C" swift::OpaqueValue *
swift_generic_initWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata) {
  const uint8_t *typeLayout = metadata->getLayoutString();
  size_t size = metadata->vw_size();

  memcpy(dest, src, size);

  if (SWIFT_LIKELY(metadata->getValueWitnesses()->isBitwiseTakable())) {
    return dest;
  }

  auto offset = layoutStringHeaderSize;
  uintptr_t addrOffset = 0;

  while (true) {
    uint64_t skip = readBytes<uint64_t>(typeLayout, offset);
    auto tag = static_cast<RefCountingKind>(skip >> 56);
    skip &= ~(0xffULL << 56);
    addrOffset += skip;

    switch (tag) {
    case RefCountingKind::UnknownWeak:
      swift_unknownObjectWeakTakeInit((WeakReference*)((uintptr_t)dest + addrOffset),
                                      (WeakReference*)((uintptr_t)src + addrOffset));
      break;
    case RefCountingKind::Metatype: {
      auto typePtr = readBytes<uintptr_t>(typeLayout, offset);
      auto *type = reinterpret_cast<Metadata*>(typePtr);
      if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
        type->vw_initializeWithTake((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                    (OpaqueValue*)((uintptr_t)src + addrOffset));
      }
      break;
    }
    case RefCountingKind::Existential: {
      auto *type = getExistentialTypeMetadata((OpaqueValue*)((uintptr_t)src + addrOffset));
      if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
        type->vw_initializeWithTake((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                    (OpaqueValue*)((uintptr_t)src + addrOffset));
      }
      break;
    }
    case RefCountingKind::Resilient: {
      auto *type = getResilientTypeMetadata(metadata, typeLayout, offset);
      if (SWIFT_UNLIKELY(!type->getValueWitnesses()->isBitwiseTakable())) {
        type->vw_initializeWithTake((OpaqueValue*)((uintptr_t)dest + addrOffset),
                                    (OpaqueValue*)((uintptr_t)src + addrOffset));
      }
      break;
    }
    case RefCountingKind::End:
      return dest;
    default:
      break;
    }
  }

  return dest;
}

extern "C" swift::OpaqueValue *
swift_generic_assignWithCopy(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata) {
  swift_generic_destroy(dest, metadata);
  return swift_generic_initWithCopy(dest, src, metadata);
}

extern "C" swift::OpaqueValue *
swift_generic_assignWithTake(swift::OpaqueValue *dest, swift::OpaqueValue *src, const Metadata *metadata) {
  swift_generic_destroy(dest, metadata);
  return swift_generic_initWithTake(dest, src, metadata);
}

extern "C" void
swift_generic_instantiateLayoutString(const uint8_t* layoutStr,
                                      Metadata* type) {
  size_t offset = 0;
  const auto refCountSize = readBytes<size_t>(layoutStr, offset);

  const size_t genericDescOffset = layoutStringHeaderSize + refCountSize + sizeof(size_t);
  offset = genericDescOffset;

  size_t genericRefCountSize = 0;
  while (true) {
    const auto tagAndOffset = readBytes<uint64_t>(layoutStr, offset);
    const auto tag = (uint8_t)(tagAndOffset >> 56);

    if (tag == 0) {
      break;
    } else if (tag == 1 || tag == 4) {
      continue;
    } else {
      const Metadata *genericType;
      if (tag == 2) {
        auto index = readBytes<uint32_t>(layoutStr, offset);
        genericType = type->getGenericArgs()[index];
      } else {
        genericType = getResilientTypeMetadata(type, layoutStr, offset);
      }

      if (genericType->getTypeContextDescriptor()->hasLayoutString()) {
        const uint8_t *genericLayoutStr = genericType->getLayoutString();
        size_t countOffset = 0;
        genericRefCountSize += readBytes<size_t>(genericLayoutStr, countOffset);
      } else if (genericType->isClassObject()) {
        genericRefCountSize += sizeof(uint64_t);
      } else {
        genericRefCountSize += sizeof(uint64_t) + sizeof(uintptr_t);
      }
    }
  }

  const auto instancedLayoutStrSize = layoutStringHeaderSize + refCountSize + genericRefCountSize + sizeof(size_t) + 1;

  uint8_t *instancedLayoutStr = (uint8_t*)calloc(instancedLayoutStrSize, sizeof(uint8_t));

  writeBytes<size_t>(instancedLayoutStr, 0, refCountSize + genericRefCountSize);

  offset = genericDescOffset;
  size_t layoutStrOffset = layoutStringHeaderSize;
  size_t instancedLayoutStrOffset = layoutStringHeaderSize;
  size_t skipBytes = 0;
  while (true) {
    const auto tagAndOffset = readBytes<uint64_t>(layoutStr, offset);
    const auto tag = (uint8_t)(tagAndOffset >> 56);
    const auto sizeOrOffset = tagAndOffset & ~(0xffULL << 56);

    if (tag == 0) {
      break;
    } else if (tag == 1) {
      memcpy((void*)(instancedLayoutStr + instancedLayoutStrOffset), (void*)(layoutStr + layoutStrOffset), sizeOrOffset);
      if (skipBytes) {
        size_t firstRCOffset = instancedLayoutStrOffset;
        auto firstRC = readBytes<uint64_t>(instancedLayoutStr, firstRCOffset);
        firstRCOffset = instancedLayoutStrOffset;
        firstRC += skipBytes;
        writeBytes(instancedLayoutStr, firstRCOffset, firstRC);
        skipBytes = 0;
      }

      layoutStrOffset += sizeOrOffset;
      instancedLayoutStrOffset += sizeOrOffset;
    } else if (tag == 4) {
      auto *alignmentType = getResilientTypeMetadata(type, layoutStr, offset);
      auto alignment = alignmentType->vw_alignment();
      auto alignmentMask = alignment - 1;
      skipBytes += sizeOrOffset;
      skipBytes += alignmentMask;
      skipBytes &= ~alignmentMask;
    } else {
      skipBytes += sizeOrOffset;
      const Metadata *genericType;
      if (tag == 2) {
        auto index = readBytes<uint32_t>(layoutStr, offset);
        genericType = type->getGenericArgs()[index];
      } else {
        genericType = getResilientTypeMetadata(type, layoutStr, offset);
      }

      if (genericType->getTypeContextDescriptor()->hasLayoutString()) {
        const uint8_t *genericLayoutStr = genericType->getLayoutString();
        size_t countOffset = 0;
        auto genericRefCountSize = readBytes<size_t>(genericLayoutStr, countOffset);
        if (genericRefCountSize > 0) {
          memcpy((void*)(instancedLayoutStr + instancedLayoutStrOffset), (void*)(genericLayoutStr + layoutStringHeaderSize), genericRefCountSize);
          if (skipBytes) {
            size_t firstRCOffset = instancedLayoutStrOffset;
            auto firstRC = readBytes<uint64_t>(instancedLayoutStr, firstRCOffset);
            firstRC += skipBytes;
            writeBytes(instancedLayoutStr, firstRCOffset, firstRC);
            skipBytes = 0;
          }

          instancedLayoutStrOffset += genericRefCountSize;
          size_t trailingBytesOffset = layoutStringHeaderSize + genericRefCountSize;
          skipBytes += readBytes<size_t>(genericLayoutStr, trailingBytesOffset);
        }
      } else if (genericType->isClassObject()) {
        uint64_t op = static_cast<uint64_t>(RefCountingKind::Unknown) << 56;
        op |= (skipBytes & ~(0xffULL << 56));

        writeBytes<uint64_t>(instancedLayoutStr, instancedLayoutStrOffset, op);

        instancedLayoutStrOffset += sizeof(uint64_t);

        skipBytes = sizeof(uintptr_t);
      } else {
        const ValueWitnessTable *vwt = genericType->getValueWitnesses();
        if (vwt->isPOD()) {
          skipBytes += vwt->getSize();
          continue;
        }

        uint64_t op = static_cast<uint64_t>(RefCountingKind::Metatype) << 56;
        op |= (skipBytes & ~(0xffULL << 56));

        writeBytes<uint64_t>(instancedLayoutStr, instancedLayoutStrOffset, op);

        instancedLayoutStrOffset += sizeof(uint64_t);

        writeBytes<uintptr_t>(instancedLayoutStr, instancedLayoutStrOffset, reinterpret_cast<uintptr_t>(genericType));
        instancedLayoutStrOffset += sizeof(uintptr_t);

        skipBytes = 0;
      }
    }
  };

  // TODO: this should not really happen once we instantiate resilient types
  if (instancedLayoutStrOffset == layoutStringHeaderSize) {
    free(instancedLayoutStr);
    type->setLayoutString(layoutStr);
    return;
  }

  size_t trailingBytesOffset = layoutStringHeaderSize + refCountSize;
  skipBytes += readBytes<uint64_t>(layoutStr, trailingBytesOffset);

  if (skipBytes > 0) {
    writeBytes<size_t>(instancedLayoutStr, layoutStringHeaderSize + refCountSize + genericRefCountSize, skipBytes);
  }

  type->setLayoutString(instancedLayoutStr);
}
