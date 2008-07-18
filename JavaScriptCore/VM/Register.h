/*
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef Register_h
#define Register_h

#include "JSValue.h"
#include <wtf/VectorTraits.h>

namespace KJS {

    class CodeBlock;
    class ExecState;
    class JSFunction;
    class JSPropertyNameIterator;
    class JSValue;
    class ScopeChainNode;
    struct Instruction;

    class Register {
    public:
        Register();
        Register(JSValue*);

        JSValue* jsValue() const;

        bool marked() const;
        void mark();
        
        bool isString() const;
        
        uint32_t toUInt32(ExecState*) const;
        UString toString(ExecState*) const;
        

    private:
        friend class Machine;

        // Only the Machine should use these functions.

        Register(CodeBlock*);
        Register(ScopeChainNode*);
        Register(intptr_t);
        Register(Register*);
        Register(Instruction*);
        Register(JSPropertyNameIterator*);

        CodeBlock* codeBlock() const;
        ScopeChainNode* scopeChain() const;
        intptr_t i() const;
        Register* r() const;
        Instruction* vPC() const;
        JSPropertyNameIterator* jsPropertyNameIterator() const;

        union {
        private:
            friend class Register;

            CodeBlock* codeBlock;
            Instruction* vPC;
            JSValue* jsValue;
            ScopeChainNode* scopeChain;
            JSPropertyNameIterator* jsPropertyNameIterator;
            Register* r;
            intptr_t i;
        } u;

#ifndef NDEBUG
        enum {
            CodeBlockType = 0, 
            InstructionType, 
            JSValueType, 
            ScopeChainNodeType, 
            JSPropertyNameIteratorType, 
            RegisterType, 
            IntType
        } m_type;
#endif
    };

    ALWAYS_INLINE Register::Register()
    {
#ifndef NDEBUG
        *this = intptr_t(0L);
#endif
    }

    ALWAYS_INLINE Register::Register(JSValue* v)
    {
#ifndef NDEBUG
        m_type = JSValueType;
#endif
        u.jsValue = v;
    }
    
    ALWAYS_INLINE Register::Register(CodeBlock* codeBlock)
    {
#ifndef NDEBUG
        m_type = CodeBlockType;
#endif
        u.codeBlock = codeBlock;
    }

    ALWAYS_INLINE Register::Register(Instruction* vPC)
    {
#ifndef NDEBUG
        m_type = InstructionType;
#endif
        u.vPC = vPC;
    }

    ALWAYS_INLINE Register::Register(ScopeChainNode* scopeChain)
    {
#ifndef NDEBUG
        m_type = ScopeChainNodeType;
#endif
        u.scopeChain = scopeChain;
    }

    ALWAYS_INLINE Register::Register(JSPropertyNameIterator* jsPropertyNameIterator)
    {
#ifndef NDEBUG
        m_type = JSPropertyNameIteratorType;
#endif
        u.jsPropertyNameIterator = jsPropertyNameIterator;
    }

    ALWAYS_INLINE Register::Register(Register* r)
    {
#ifndef NDEBUG
        m_type = RegisterType;
#endif
        u.r = r;
    }

    ALWAYS_INLINE Register::Register(intptr_t i)
    {
#ifndef NDEBUG
        m_type = IntType;
#endif
        u.i = i;
    }

    ALWAYS_INLINE JSValue* Register::jsValue() const
    {
        ASSERT(m_type == JSValueType || !i());
        return u.jsValue;
    }
    
    ALWAYS_INLINE CodeBlock* Register::codeBlock() const
    {
        ASSERT(m_type == CodeBlockType);
        return u.codeBlock;
    }
    
    ALWAYS_INLINE ScopeChainNode* Register::scopeChain() const
    {
        ASSERT(m_type == ScopeChainNodeType);
        return u.scopeChain;
    }
    
    ALWAYS_INLINE intptr_t Register::i() const
    {
        ASSERT(m_type == IntType);
        return u.i;
    }
    
    ALWAYS_INLINE Register* Register::r() const
    {
        ASSERT(m_type == RegisterType);
        return u.r;
    }
    
    ALWAYS_INLINE Instruction* Register::vPC() const
    {
        ASSERT(m_type == InstructionType);
        return u.vPC;
    }
    
    ALWAYS_INLINE JSPropertyNameIterator* Register::jsPropertyNameIterator() const
    {
        ASSERT(m_type == JSPropertyNameIteratorType);
        return u.jsPropertyNameIterator;
    }
    
    ALWAYS_INLINE bool Register::marked() const
    {
        return jsValue()->marked();
    }

    ALWAYS_INLINE void Register::mark()
    {
        jsValue()->mark();
    }
    
    ALWAYS_INLINE bool Register::isString() const
    {
        return jsValue()->isString();
    }

    ALWAYS_INLINE uint32_t Register::toUInt32(ExecState* exec) const
    {
        return jsValue()->toUInt32(exec);
    }

    ALWAYS_INLINE UString Register::toString(ExecState* exec) const
    {
        return jsValue()->toString(exec);
    }

} // namespace KJS

namespace WTF {

    template<> struct VectorTraits<KJS::Register> : VectorTraitsBase<true, KJS::Register> { };

} // namespace WTF

#endif // Register_h
