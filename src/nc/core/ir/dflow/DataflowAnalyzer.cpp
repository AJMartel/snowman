/* The file is part of Snowman decompiler.             */
/* See doc/licenses.txt for the licensing information. */

//
// SmartDec decompiler - SmartDec is a native code to C/C++ decompiler
// Copyright (C) 2015 Alexander Chernov, Katerina Troshina, Yegor Derevenets,
// Alexander Fokin, Sergey Levin, Leonid Tsvetkov
//
// This file is part of SmartDec decompiler.
//
// SmartDec decompiler is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SmartDec decompiler is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SmartDec decompiler.  If not, see <http://www.gnu.org/licenses/>.
//

#include "DataflowAnalyzer.h"

#include <boost/unordered_map.hpp>

#include <nc/common/CancellationToken.h>
#include <nc/common/Foreach.h>
#include <nc/common/Warnings.h>

#include <nc/core/arch/Architecture.h>
#include <nc/core/arch/Instruction.h>
#include <nc/core/arch/Register.h>
#include <nc/core/ir/BasicBlock.h>
#include <nc/core/ir/CFG.h>
#include <nc/core/ir/Function.h>
#include <nc/core/ir/Jump.h>
#include <nc/core/ir/Statements.h>
#include <nc/core/ir/Terms.h>
#include <nc/core/ir/calls/CallAnalyzer.h>
#include <nc/core/ir/calls/CallsData.h>
#include <nc/core/ir/calls/FunctionAnalyzer.h>
#include <nc/core/ir/calls/ReturnAnalyzer.h>
#include <nc/core/ir/misc/CensusVisitor.h>

#include "Dataflow.h"
#include "SimulationContext.h"

namespace nc {
namespace core {
namespace ir {
namespace dflow {

void DataflowAnalyzer::analyze(const Function *function, const CancellationToken &canceled) {
    CFG cfg(function->basicBlocks());

    /*
     * Run simulation until reaching stationary point twice in a row.
     */
    boost::unordered_map<const BasicBlock *, ReachingDefinitions> outputDefinitions;

    int niterations = 0;
    bool changed;
    bool fixpointReached = false;

    do {
        changed = false;

        /*
         * Compute definitions.
         */
        foreach (const BasicBlock *basicBlock, function->basicBlocks()) {
            SimulationContext context(*this, function, fixpointReached);

            /* Merge the reaching definitions from predecessors. */
            foreach (const BasicBlock *predecessor, cfg.getPredecessors(basicBlock)) {
                context.definitions().merge(outputDefinitions[predecessor]);
            }

            /* If this is a function entry, run the calling convention-specific code. */
            if (basicBlock == function->entry()) {
                if (callsData()) {
                    if (calls::FunctionAnalyzer *functionAnalyzer = callsData()->getFunctionAnalyzer(function)) {
                        functionAnalyzer->simulateEnter(context);
                    }
                }
            }

            /* Simulate all the statements in the basic block. */
            foreach (const Statement *statement, basicBlock->statements()) {
                simulate(statement, context);
            }

            /* Something changed? */
            ReachingDefinitions &definitions(outputDefinitions[basicBlock]);
            if (definitions != context.definitions()) {
                definitions = context.definitions();
                changed = true;
            }
        }

        /*
         * Compute uses.
         */
        misc::CensusVisitor census(callsData());
        census(function);

        foreach (const Term *term, census.terms()) {
            if (term->isWrite()) {
                dataflow().clearUses(term);
            }
        }

        foreach (const Term *term, census.terms()) {
            if (term->isRead()) {
                foreach (const auto &def, dataflow().getDefinitions(term).definitions()) {
                    foreach (const Term *definition, def.second) {
                        dataflow().addUse(definition, term);
                    }
                }
            }
        }

        /*
         * Have we reached a fixpoint?
         */
        if (changed) {
            fixpointReached = false;
        } else if (!fixpointReached) {
            fixpointReached = true;
            changed = true;
        }

        /*
         * Do we loop infinitely?
         */
        if (++niterations >= 30) {
            ncWarning("Didn't reach a fixpoint after %1 iterations while analyzing dataflow of %2. Giving up.", niterations, function->name());
            break;
        }
    } while (changed && !canceled);
}

void DataflowAnalyzer::simulate(const Statement *statement, SimulationContext &context) {
    switch (statement->kind()) {
        case Statement::COMMENT:
            break;
        case Statement::INLINE_ASSEMBLY:
            /*
             * To be completely correct, one should clear reaching definitions.
             * However, not doing this usually leads to better code.
             */
            break;
        case Statement::ASSIGNMENT: {
            const Assignment *assignment = statement->asAssignment();
            simulate(assignment->right(), context);
            simulate(assignment->left(), context);
            break;
        }
        case Statement::KILL: {
            const Kill *kill = statement->asKill();
            simulate(kill->term(), context);
            break;
        }
        case Statement::JUMP: {
            const Jump *jump = statement->asJump();

            if (jump->condition()) {
                simulate(jump->condition(), context);
            }
            if (jump->thenTarget().address()) {
                simulate(jump->thenTarget().address(), context);
            }
            if (jump->elseTarget().address()) {
                simulate(jump->elseTarget().address(), context);
            }
            break;
        }
        case Statement::CALL: {
            const Call *call = statement->asCall();
            simulate(call->target(), context);

            if (callsData()) {
                const Value *targetValue = dataflow().getValue(call->target());
                if (targetValue->abstractValue().isConcrete()) {
                    callsData()->setCalledAddress(call, targetValue->abstractValue().asConcrete().value());
                }
                if (calls::CallAnalyzer *callAnalyzer = callsData()->getCallAnalyzer(call)) {
                    callAnalyzer->simulateCall(context);
                }
            }
            break;
        }
        case Statement::RETURN: {
            if (callsData() && context.function()) {
                if (calls::ReturnAnalyzer *returnAnalyzer = callsData()->getReturnAnalyzer(context.function(), statement->asReturn())) {
                    returnAnalyzer->simulateReturn(context);
                }
            }
            break;
        }
        default:
            ncWarning("Was called for unsupported kind of statement: '%1'.", static_cast<int>(statement->kind()));
            break;
    }
}

void DataflowAnalyzer::simulate(const Term *term, SimulationContext &context) {
    assert(term != NULL);

    switch (term->kind()) {
        case Term::INT_CONST: {
            const Constant *constant = term->asConstant();

            Value *value = dataflow().getValue(constant);
            value->setAbstractValue(constant->value());
            value->makeNotStackOffset();
            value->makeNotProduct();
            break;
        }
        case Term::INTRINSIC: /* FALLTHROUGH */
        case Term::UNDEFINED: {
            Value *value = dataflow().getValue(term);
            value->setAbstractValue(AbstractValue(term->size(), -1, -1));
            value->makeNotStackOffset();
            value->makeNotProduct();
            break;
        }
        case Term::MEMORY_LOCATION_ACCESS: {
            const MemoryLocationAccess *access = term->asMemoryLocationAccess();
            dataflow().setMemoryLocation(access, access->memoryLocation());

            /* The value of instruction pointer is always easy to guess. */
            if (architecture()->instructionPointer() &&
                access->memoryLocation() == architecture()->instructionPointer()->memoryLocation() &&
                access->statement() &&
                access->statement()->instruction())
            {
                dataflow().getValue(access)->setAbstractValue(
                    SizedValue(term->size(), access->statement()->instruction()->addr()));
            }
            break;
        }
        case Term::DEREFERENCE: {
            const Dereference *dereference = term->asDereference();

            simulate(dereference->address(), context);

            const Value *addressValue = dataflow().getValue(dereference->address());
            if (addressValue->abstractValue().isConcrete()) {
                if (dereference->domain() == MemoryDomain::MEMORY) {
                    dataflow().setMemoryLocation(dereference, MemoryLocation(dereference->domain(),
                        addressValue->abstractValue().asConcrete().value() * CHAR_BIT, dereference->size()));
                } else {
                    dataflow().setMemoryLocation(dereference, MemoryLocation(dereference->domain(),
                        addressValue->abstractValue().asConcrete().value(), dereference->size()));
                }
            } else if (addressValue->isStackOffset()) {
                dataflow().setMemoryLocation(dereference,
                    MemoryLocation(MemoryDomain::STACK, addressValue->stackOffset().signedValue() * CHAR_BIT, dereference->size()));
            } else {
                dataflow().unsetMemoryLocation(dereference);
            }
            break;
        }
        case Term::UNARY_OPERATOR:
            simulateUnaryOperator(term->asUnaryOperator(), context);
            break;
        case Term::BINARY_OPERATOR:
            simulateBinaryOperator(term->asBinaryOperator(), context);
            break;
        case Term::CHOICE: {
            const Choice *choice = term->asChoice();
            simulate(choice->preferredTerm(), context);
            simulate(choice->defaultTerm(), context);

            if (!dataflow().getDefinitions(choice->preferredTerm()).empty()) {
                *dataflow().getValue(choice) = *dataflow().getValue(choice->preferredTerm());
            } else {
                *dataflow().getValue(choice) = *dataflow().getValue(choice->defaultTerm());
            }
            break;
        }
        default:
            ncWarning("Was called for unsupported kind of term: '%1'.", static_cast<int>(term->kind()));
            break;
    }

    if (const MemoryLocation &termLocation = dataflow().getMemoryLocation(term)) {
        if (!architecture()->isGlobalMemory(termLocation)) {
            if (term->isRead()) {
                auto definitions = context.definitions().getDefinitions(termLocation);
                dataflow().setDefinitions(term, definitions);

                Value *termValue = dataflow().getValue(term);
                auto termAbstractValue = termValue->abstractValue();

                foreach (const auto &def, definitions.definitions()) {
                    auto &definedLocation = def.first;

                    assert(termLocation.covers(definedLocation));

                    foreach (const Term *definition, def.second) {
                        auto definitionLocation = dataflow().getMemoryLocation(definition);

                        if (!definitionLocation.covers(definedLocation)) {
                            continue;
                        }

                        auto definitionValue = dataflow().getValue(definition);
                        auto definitionAbstractValue = definitionValue->abstractValue();

                        /*
                         * Shift definition's abstract value to match term's location.
                         */
                        if (architecture()->byteOrder() == arch::ByteOrder::LittleEndian) {
                            definitionAbstractValue.shift(definitionLocation.addr() - termLocation.addr());
                        } else {
                            definitionAbstractValue.shift(termLocation.endAddr() - definitionLocation.endAddr());
                        }

                        /*
                         * Project the value to the defined location.
                         */
                        auto mask = bitMask<ConstantValue>(definedLocation.size());
                        if (architecture()->byteOrder() == arch::ByteOrder::LittleEndian) {
                            mask = bitShift(mask, definedLocation.addr() - termLocation.addr());
                        } else {
                            mask = bitShift(mask, termLocation.endAddr() - definedLocation.endAddr());
                        }
                        definitionAbstractValue.project(mask);

                        termAbstractValue.merge(definitionAbstractValue);
                    }
                }

                termValue->setAbstractValue(termAbstractValue.resize(term->size()));

                // TODO: product, stack offset.
            }
            if (term->isWrite()) {
                context.definitions().addDefinition(termLocation, term);
            }
            if (term->isKill()) {
                context.definitions().killDefinitions(termLocation);
            }
        } else {
            if (term->isRead()) {
                dataflow().clearDefinitions(term);
            }
        }
    } else {
        if (term->isRead()) {
            dataflow().clearDefinitions(term);
        }
    }
}

void DataflowAnalyzer::simulateUnaryOperator(const UnaryOperator *unary, SimulationContext &context) {
    simulate(unary->operand(), context);

    Value *value = dataflow().getValue(unary);
    Value *operandValue = dataflow().getValue(unary->operand());

    value->setAbstractValue(unary->apply(operandValue->abstractValue()));

    switch (unary->operatorKind()) {
        case UnaryOperator::SIGN_EXTEND:
        case UnaryOperator::ZERO_EXTEND:
        case UnaryOperator::TRUNCATE:
            if (operandValue->isStackOffset()) {
                value->makeStackOffset(operandValue->stackOffset());
            } else if (operandValue->isNotStackOffset()) {
                value->makeNotStackOffset();
            }
            if (operandValue->isProduct()) {
                value->makeProduct();
            } else if (operandValue->isNotProduct()) {
                value->makeNotProduct();
            }
            break;
        default:
            value->makeNotStackOffset();
            value->makeNotProduct();
            break;
    }
}

void DataflowAnalyzer::simulateBinaryOperator(const BinaryOperator *binary, SimulationContext &context) {
    simulate(binary->left(), context);
    simulate(binary->right(), context);

    Value *value = dataflow().getValue(binary);
    Value *leftValue = dataflow().getValue(binary->left());
    Value *rightValue = dataflow().getValue(binary->right());

    value->setAbstractValue(binary->apply(leftValue->abstractValue(), rightValue->abstractValue()));

    /* Compute stack offset. */
    switch (binary->operatorKind()) {
        case BinaryOperator::ADD:
            if (leftValue->abstractValue().isConcrete()) {
                if (rightValue->isStackOffset()) {
                    value->makeStackOffset(leftValue->abstractValue().asConcrete().signedValue() + rightValue->stackOffset().signedValue());
                } else if (rightValue->isNotStackOffset()) {
                    value->makeNotStackOffset();
                }
            } else if (leftValue->abstractValue().isNondeterministic()) {
                value->makeNotStackOffset();
            }
            if (rightValue->abstractValue().isConcrete()) {
                if (leftValue->isStackOffset()) {
                    value->makeStackOffset(leftValue->stackOffset().signedValue() + rightValue->abstractValue().asConcrete().signedValue());
                } else if (leftValue->isNotStackOffset()) {
                    value->makeNotStackOffset();
                }
            } else if (rightValue->abstractValue().isNondeterministic()) {
                value->makeNotStackOffset();
            }
            break;

        case BinaryOperator::SUB:
            if (leftValue->isStackOffset() && rightValue->abstractValue().isConcrete()) {
                value->makeStackOffset(leftValue->stackOffset().signedValue() - rightValue->abstractValue().asConcrete().signedValue());
            } else if (leftValue->isNotStackOffset() || rightValue->abstractValue().isNondeterministic()) {
                value->makeNotStackOffset();
            }
            break;

        case BinaryOperator::AND:
            /* Sometimes used for getting aligned stack pointer values. */
            if (leftValue->isStackOffset() && rightValue->abstractValue().isConcrete()) {
                value->makeStackOffset(leftValue->stackOffset().value() & rightValue->abstractValue().asConcrete().value());
            } else if (rightValue->isStackOffset() && leftValue->abstractValue().isConcrete()) {
                value->makeStackOffset(rightValue->stackOffset().value() & leftValue->abstractValue().asConcrete().value());
            } else if ((leftValue->abstractValue().isNondeterministic() && leftValue->isNotStackOffset()) ||
                       (rightValue->abstractValue().isNondeterministic() && rightValue->isNotStackOffset())) {
                value->makeNotStackOffset();
            }
            break;

        default:
            value->makeNotStackOffset();
            break;
    }

    /* Compute product flag. */
    switch (binary->operatorKind()) {
        case BinaryOperator::MUL:
        case BinaryOperator::SHL:
            value->makeProduct();
            break;
        default:
            value->makeNotProduct();
            break;
    }
}

} // namespace dflow
} // namespace ir
} // namespace core
} // namespace nc

/* vim:set et sts=4 sw=4: */
