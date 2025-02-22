// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.
//
//                                    Early Value Propagation
//
// This phase performs an SSA-based value propagation optimization that currently only applies to array
// lengths, runtime type handles, and explicit null checks. An SSA-based backwards tracking of local variables
// is performed at each point of interest, e.g., an array length reference site, a method table reference site, or
// an indirection.
// The tracking continues until an interesting value is encountered. The value is then used to rewrite
// the source site or the value.
//
///////////////////////////////////////////////////////////////////////////////////////

#include "jitpch.h"
#include "ssabuilder.h"

bool Compiler::optDoEarlyPropForFunc()
{
    bool propArrayLen  = (optMethodFlags & OMF_HAS_NEWARRAY) && (optMethodFlags & OMF_HAS_ARRAYREF);
    bool propGetType   = (optMethodFlags & OMF_HAS_NEWOBJ) && (optMethodFlags & OMF_HAS_VTABLEREF);
    bool propNullCheck = (optMethodFlags & OMF_HAS_NULLCHECK) != 0;
    return propArrayLen || propGetType || propNullCheck;
}

bool Compiler::optDoEarlyPropForBlock(BasicBlock* block)
{
    bool bbHasArrayRef  = (block->bbFlags & BBF_HAS_IDX_LEN) != 0;
    bool bbHasVtableRef = (block->bbFlags & BBF_HAS_VTABREF) != 0;
    bool bbHasNullCheck = (block->bbFlags & BBF_HAS_NULLCHECK) != 0;
    return bbHasArrayRef || bbHasVtableRef || bbHasNullCheck;
}

//--------------------------------------------------------------------
// gtIsVtableRef: Return true if the tree is a method table reference.
//
// Arguments:
//    tree           - The input tree.
//
// Return Value:
//    Return true if the tree is a method table reference.

bool Compiler::gtIsVtableRef(GenTree* tree)
{
    if (tree->OperGet() == GT_IND)
    {
        GenTree* addr = tree->AsIndir()->Addr();

        if (addr->OperIsAddrMode())
        {
            GenTreeAddrMode* addrMode = addr->AsAddrMode();

            return (!addrMode->HasIndex() && (addrMode->Base()->TypeGet() == TYP_REF));
        }
    }

    return false;
}

//------------------------------------------------------------------------------
// getArrayLengthFromAllocation: Return the array length for an array allocation
//                               helper call.
//
// Arguments:
//    tree           - The array allocation helper call.
//
// Return Value:
//    Return the array length node.

GenTree* Compiler::getArrayLengthFromAllocation(GenTree* tree)
{
    assert(tree != nullptr);

    if (tree->OperGet() == GT_CALL)
    {
        GenTreeCall* call = tree->AsCall();

        if (call->gtCallType == CT_HELPER)
        {
            if (call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_R2R_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_OBJ) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_VC) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_ALIGN8))
            {
                // This is an array allocation site. Grab the array length node.
                return gtArgEntryByArgNum(call, 1)->node;
            }
        }
    }

    return nullptr;
}

//-----------------------------------------------------------------------------
// getObjectHandleNodeFromAllocation: Return the type handle for an object allocation
//                              helper call.
//
// Arguments:
//    tree           - The object allocation helper call.
//
// Return Value:
//    Return the object type handle node.

GenTree* Compiler::getObjectHandleNodeFromAllocation(GenTree* tree)
{
    assert(tree != nullptr);

    if (tree->OperGet() == GT_CALL)
    {
        GenTreeCall* call = tree->AsCall();

        if (call->gtCallType == CT_HELPER)
        {
            if (call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWFAST) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_FINALIZE) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_ALIGN8) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_ALIGN8_VC) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWSFAST_ALIGN8_FINALIZE) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_R2R_DIRECT) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_OBJ) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_VC) ||
                call->gtCallMethHnd == eeFindHelper(CORINFO_HELP_NEWARR_1_ALIGN8))
            {
                // This is an object allocation site. Return the runtime type handle node.
                fgArgTabEntry* argTabEntry = gtArgEntryByArgNum(call, 0);
                return argTabEntry->node;
            }
        }
    }

    return nullptr;
}

//------------------------------------------------------------------------------------------
// optEarlyProp: The entry point of the early value propagation.
//
// Notes:
//    This phase performs an SSA-based value propagation, including
//      1. Array length propagation.
//      2. Runtime type handle propagation.
//      3. Null check folding.
//
//    For array length propagation, a demand-driven SSA-based backwards tracking of constant
//    array lengths is performed at each array length reference site which is in form of a
//    GT_ARR_LENGTH node. When a GT_ARR_LENGTH node is seen, the array ref pointer which is
//    the only child node of the GT_ARR_LENGTH is tracked. This is only done for array ref
//    pointers that have valid SSA forms.The tracking is along SSA use-def chain and stops
//    at the original array allocation site where we can grab the array length. The
//    GT_ARR_LENGTH node will then be rewritten to a GT_CNS_INT node if the array length is
//    constant.
//
//    Similarly, the same algorithm also applies to rewriting a method table (also known as
//    vtable) reference site which is in form of GT_INDIR node. The base pointer, which is
//    an object reference pointer, is treated in the same way as an array reference pointer.
//
//    Null check folding tries to find GT_INDIR(obj + const) that GT_NULLCHECK(obj) can be folded into
//    and removed. Currently, the algorithm only matches GT_INDIR and GT_NULLCHECK in the same basic block.

void Compiler::optEarlyProp()
{
#ifdef DEBUG
    if (verbose)
    {
        printf("*************** In optEarlyProp()\n");
    }
#endif

    assert(fgSsaPassesCompleted == 1);

    if (!optDoEarlyPropForFunc())
    {
        return;
    }

    for (BasicBlock* block = fgFirstBB; block != nullptr; block = block->bbNext)
    {
        if (!optDoEarlyPropForBlock(block))
        {
            continue;
        }

        compCurBB = block;

        for (Statement* stmt = block->firstStmt(); stmt != nullptr;)
        {
            // Preserve the next link before the propagation and morph.
            Statement* next = stmt->GetNextStmt();

            compCurStmt = stmt;

            // Walk the stmt tree in linear order to rewrite any array length reference with a
            // constant array length.
            bool isRewritten = false;
            for (GenTree* tree = stmt->gtStmtList; tree != nullptr; tree = tree->gtNext)
            {
                GenTree* rewrittenTree = optEarlyPropRewriteTree(tree);
                if (rewrittenTree != nullptr)
                {
                    gtUpdateSideEffects(stmt, rewrittenTree);
                    isRewritten = true;
                    tree        = rewrittenTree;
                }
            }

            // Update the evaluation order and the statement info if the stmt has been rewritten.
            if (isRewritten)
            {
                gtSetStmtInfo(stmt);
                fgSetStmtSeq(stmt);
            }

            stmt = next;
        }
    }

#ifdef DEBUG
    if (verbose)
    {
        JITDUMP("\nAfter optEarlyProp:\n");
        fgDispBasicBlocks(/*dumpTrees*/ true);
    }
#endif
}

//----------------------------------------------------------------
// optEarlyPropRewriteValue: Rewrite a tree to the actual value.
//
// Arguments:
//    tree           - The input tree node to be rewritten.
//
// Return Value:
//    Return a new tree if the original tree was successfully rewritten.
//    The containing tree links are updated.
//
GenTree* Compiler::optEarlyPropRewriteTree(GenTree* tree)
{
    GenTree*    objectRefPtr = nullptr;
    optPropKind propKind     = optPropKind::OPK_INVALID;

    if (tree->OperGet() == GT_ARR_LENGTH)
    {
        objectRefPtr = tree->gtOp.gtOp1;
        propKind     = optPropKind::OPK_ARRAYLEN;
    }
    else if (tree->OperIsIndir())
    {
        // optFoldNullCheck takes care of updating statement info if a null check is removed.
        optFoldNullCheck(tree);

        if (gtIsVtableRef(tree))
        {
            // Don't propagate type handles that are used as null checks, which are usually in
            // form of
            //      *  stmtExpr  void  (top level)
            //      \--*  indir     int
            //          \--*  lclVar    ref    V02 loc0
            if (compCurStmt->gtStmtExpr == tree)
            {
                return nullptr;
            }

            objectRefPtr = tree->AsIndir()->Addr();
            propKind     = optPropKind::OPK_OBJ_GETTYPE;
        }
        else
        {
            return nullptr;
        }
    }
    else
    {
        return nullptr;
    }

    if (!objectRefPtr->OperIsScalarLocal() || !lvaInSsa(objectRefPtr->AsLclVarCommon()->GetLclNum()))

    {
        return nullptr;
    }

    unsigned lclNum    = objectRefPtr->AsLclVarCommon()->GetLclNum();
    unsigned ssaNum    = objectRefPtr->AsLclVarCommon()->GetSsaNum();
    GenTree* actualVal = optPropGetValue(lclNum, ssaNum, propKind);

    if (actualVal != nullptr)
    {
        assert((propKind == optPropKind::OPK_ARRAYLEN) || (propKind == optPropKind::OPK_OBJ_GETTYPE));
        assert(actualVal->IsCnsIntOrI());
        assert(actualVal->GetNodeSize() == TREE_NODE_SZ_SMALL);

        ssize_t actualConstVal = actualVal->AsIntCon()->IconValue();

        if (propKind == optPropKind::OPK_ARRAYLEN)
        {
            if ((actualConstVal < 0) || (actualConstVal > INT32_MAX))
            {
                // Don't propagate array lengths that are beyond the maximum value of a GT_ARR_LENGTH or negative.
                // node. CORINFO_HELP_NEWARR_1_OBJ helper call allows to take a long integer as the
                // array length argument, but the type of GT_ARR_LENGTH is always INT32.
                return nullptr;
            }

            // When replacing GT_ARR_LENGTH nodes with constants we can end up with GT_ARR_BOUNDS_CHECK
            // nodes that have constant operands and thus can be trivially proved to be useless. It's
            // better to remove these range checks here, otherwise they'll pass through assertion prop
            // (creating useless (c1 < c2)-like assertions) and reach RangeCheck where they are finally
            // removed. Common patterns like new int[] { x, y, z } benefit from this.

            if ((tree->gtNext != nullptr) && tree->gtNext->OperIs(GT_ARR_BOUNDS_CHECK))
            {
                GenTreeBoundsChk* check = tree->gtNext->AsBoundsChk();

                if ((check->gtArrLen == tree) && check->gtIndex->IsCnsIntOrI())
                {
                    ssize_t checkConstVal = check->gtIndex->AsIntCon()->IconValue();
                    if ((checkConstVal >= 0) && (checkConstVal < actualConstVal))
                    {
                        GenTree* comma = check->gtGetParent(nullptr);
                        if ((comma != nullptr) && comma->OperIs(GT_COMMA) && (comma->gtGetOp1() == check))
                        {
                            optRemoveRangeCheck(comma, compCurStmt);
                            // Both `tree` and `check` have been removed from the statement.
                            // 'tree' was replaced with 'nop' or side effect list under 'comma'.
                            return comma->gtGetOp1();
                        }
                    }
                }
            }
        }

#ifdef DEBUG
        if (verbose)
        {
            printf("optEarlyProp Rewriting " FMT_BB "\n", compCurBB->bbNum);
            gtDispStmt(compCurStmt);
            printf("\n");
        }
#endif

        GenTree* actualValClone = gtCloneExpr(actualVal);

        if (actualValClone->gtType != tree->gtType)
        {
            assert(actualValClone->gtType == TYP_LONG);
            assert(tree->gtType == TYP_INT);
            assert((actualConstVal >= 0) && (actualConstVal <= INT32_MAX));
            actualValClone->gtType = tree->gtType;
        }

        // Propagating a constant into an array index expression requires calling
        // LabelIndex to update the FieldSeq annotations.  EarlyProp may replace
        // array length expressions with constants, so check if this is an array
        // length operator that is part of an array index expression.
        bool isIndexExpr = (tree->OperGet() == GT_ARR_LENGTH && ((tree->gtFlags & GTF_ARRLEN_ARR_IDX) != 0));
        if (isIndexExpr)
        {
            actualValClone->LabelIndex(this);
        }

        // actualValClone has small tree node size, it is safe to use CopyFrom here.
        tree->ReplaceWith(actualValClone, this);

#ifdef DEBUG
        if (verbose)
        {
            printf("to\n");
            gtDispStmt(compCurStmt);
            printf("\n");
        }
#endif
        return tree;
    }

    return nullptr;
}

//-------------------------------------------------------------------------------------------
// optPropGetValue: Given an SSA object ref pointer, get the value needed based on valueKind.
//
// Arguments:
//    lclNum         - The local var number of the ref pointer.
//    ssaNum         - The SSA var number of the ref pointer.
//    valueKind      - The kind of value of interest.
//
// Return Value:
//    Return the corresponding value based on valueKind.

GenTree* Compiler::optPropGetValue(unsigned lclNum, unsigned ssaNum, optPropKind valueKind)
{
    return optPropGetValueRec(lclNum, ssaNum, valueKind, 0);
}

//-----------------------------------------------------------------------------------
// optPropGetValueRec: Given an SSA object ref pointer, get the value needed based on valueKind
//                     within a recursion bound.
//
// Arguments:
//    lclNum         - The local var number of the array pointer.
//    ssaNum         - The SSA var number of the array pointer.
//    valueKind      - The kind of value of interest.
//    walkDepth      - Current recursive walking depth.
//
// Return Value:
//    Return the corresponding value based on valueKind.

GenTree* Compiler::optPropGetValueRec(unsigned lclNum, unsigned ssaNum, optPropKind valueKind, int walkDepth)
{
    if (ssaNum == SsaConfig::RESERVED_SSA_NUM)
    {
        return nullptr;
    }

    SSAName  ssaName(lclNum, ssaNum);
    GenTree* value = nullptr;

    // Bound the recursion with a hard limit.
    if (walkDepth > optEarlyPropRecurBound)
    {
        return nullptr;
    }

    // Track along the use-def chain to get the array length
    GenTree* treelhs = lvaTable[lclNum].GetPerSsaData(ssaNum)->m_defLoc.m_tree;

    if (treelhs == nullptr)
    {
        // Incoming parameters or live-in variables don't have actual definition tree node
        // for their FIRST_SSA_NUM. See SsaBuilder::RenameVariables.
        assert(ssaNum == SsaConfig::FIRST_SSA_NUM);
    }
    else
    {
        GenTree** lhsPtr;
        GenTree*  treeDefParent = treelhs->gtGetParent(&lhsPtr);

        if (treeDefParent->OperGet() == GT_ASG)
        {
            assert(treelhs == treeDefParent->gtGetOp1());
            GenTree* treeRhs = treeDefParent->gtGetOp2();

            if (treeRhs->OperIsScalarLocal() && lvaInSsa(treeRhs->AsLclVarCommon()->GetLclNum()))
            {
                // Recursively track the Rhs
                unsigned rhsLclNum = treeRhs->AsLclVarCommon()->GetLclNum();
                unsigned rhsSsaNum = treeRhs->AsLclVarCommon()->GetSsaNum();

                value = optPropGetValueRec(rhsLclNum, rhsSsaNum, valueKind, walkDepth + 1);
            }
            else
            {
                if (valueKind == optPropKind::OPK_ARRAYLEN)
                {
                    value = getArrayLengthFromAllocation(treeRhs);
                    if (value != nullptr)
                    {
                        if (!value->IsCnsIntOrI())
                        {
                            // Leave out non-constant-sized array
                            value = nullptr;
                        }
                    }
                }
                else if (valueKind == optPropKind::OPK_OBJ_GETTYPE)
                {
                    value = getObjectHandleNodeFromAllocation(treeRhs);
                    if (value != nullptr)
                    {
                        if (!value->IsCnsIntOrI())
                        {
                            // Leave out non-constant-sized array
                            value = nullptr;
                        }
                    }
                }
            }
        }
    }

    return value;
}

//----------------------------------------------------------------
// optFoldNullChecks: Try to find a GT_NULLCHECK node that can be folded into the GT_INDIR node.
//
// Arguments:
//    tree           - The input GT_INDIR tree.
//

void Compiler::optFoldNullCheck(GenTree* tree)
{
    //
    // Check for a pattern like this:
    //
    //                         =
    //                       /   \.
    //                      x    comma
    //                           /   \.
    //                     nullcheck  +
    //                         |     / \.
    //                         y    y  const
    //
    //
    //                    some trees in the same
    //                    basic block with
    //                    no unsafe side effects
    //
    //                           indir
    //                             |
    //                             x
    //
    // where the const is suitably small
    // and transform it into
    //
    //                         =
    //                       /   \.
    //                      x     +
    //                           / \.
    //                          y  const
    //
    //
    //              some trees with no unsafe side effects here
    //
    //                           indir
    //                             |
    //                             x

    if ((compCurBB->bbFlags & BBF_HAS_NULLCHECK) == 0)
    {
        return;
    }

    assert(tree->OperIsIndir());

    GenTree* const addr = tree->AsIndir()->Addr();
    if (addr->OperGet() == GT_LCL_VAR)
    {
        // Check if we have the pattern above and find the nullcheck node if we do.

        // Find the definition of the indirected local (x in the picture)
        GenTreeLclVarCommon* const lclVarNode = addr->AsLclVarCommon();

        const unsigned lclNum = lclVarNode->GetLclNum();
        const unsigned ssaNum = lclVarNode->GetSsaNum();

        if (ssaNum != SsaConfig::RESERVED_SSA_NUM)
        {
            DefLoc      defLoc   = lvaTable[lclNum].GetPerSsaData(ssaNum)->m_defLoc;
            BasicBlock* defBlock = defLoc.m_blk;

            if (compCurBB == defBlock)
            {
                GenTree* defTree   = defLoc.m_tree;
                GenTree* defParent = defTree->gtGetParent(nullptr);

                if ((defParent->OperGet() == GT_ASG) && (defParent->gtNext == nullptr))
                {
                    GenTree* defRHS = defParent->gtGetOp2();
                    if (defRHS->OperGet() == GT_COMMA)
                    {
                        if (defRHS->gtGetOp1()->OperGet() == GT_NULLCHECK)
                        {
                            GenTree* nullCheckTree = defRHS->gtGetOp1();
                            if (nullCheckTree->gtGetOp1()->OperGet() == GT_LCL_VAR)
                            {
                                // We found a candidate for 'y' in the picture
                                unsigned nullCheckLclNum = nullCheckTree->gtGetOp1()->AsLclVarCommon()->GetLclNum();

                                if (defRHS->gtGetOp2()->OperGet() == GT_ADD)
                                {
                                    GenTree* additionNode = defRHS->gtGetOp2();
                                    if ((additionNode->gtGetOp1()->OperGet() == GT_LCL_VAR) &&
                                        (additionNode->gtGetOp1()->gtLclVarCommon.GetLclNum() == nullCheckLclNum))
                                    {
                                        GenTree* offset = additionNode->gtGetOp2();
                                        if (offset->IsCnsIntOrI())
                                        {
                                            if (!fgIsBigOffset(offset->gtIntConCommon.IconValue()))
                                            {
                                                // Walk from the use to the def in reverse execution order to see
                                                // if any nodes have unsafe side effects.
                                                GenTree*       currentTree        = lclVarNode->gtPrev;
                                                bool           isInsideTry        = compCurBB->hasTryIndex();
                                                bool           canRemoveNullCheck = true;
                                                const unsigned maxNodesWalked     = 25;
                                                unsigned       nodesWalked        = 0;

                                                // First walk the nodes in the statement containing the indirection
                                                // in reverse execution order starting with the indirection's
                                                // predecessor.
                                                while (canRemoveNullCheck && (currentTree != nullptr))
                                                {
                                                    if ((nodesWalked++ > maxNodesWalked) ||
                                                        !optCanMoveNullCheckPastTree(currentTree, isInsideTry))
                                                    {
                                                        canRemoveNullCheck = false;
                                                    }
                                                    else
                                                    {
                                                        currentTree = currentTree->gtPrev;
                                                    }
                                                }

                                                // Then walk the statement list in reverse execution order
                                                // until we get to the statement containing the null check.
                                                // We only need to check the side effects at the root of each statement.
                                                Statement* curStmt = compCurStmt->getPrevStmt();
                                                currentTree        = curStmt->gtStmtExpr;
                                                while (canRemoveNullCheck && (currentTree != defParent))
                                                {
                                                    if ((nodesWalked++ > maxNodesWalked) ||
                                                        !optCanMoveNullCheckPastTree(currentTree, isInsideTry))
                                                    {
                                                        canRemoveNullCheck = false;
                                                    }
                                                    else
                                                    {
                                                        curStmt = curStmt->getPrevStmt();
                                                        assert(curStmt != nullptr);
                                                        currentTree = curStmt->gtStmtExpr;
                                                    }
                                                }

                                                if (canRemoveNullCheck)
                                                {
                                                    // Remove the null check
                                                    nullCheckTree->gtFlags &= ~(GTF_EXCEPT | GTF_DONT_CSE);

                                                    // Set this flag to prevent reordering
                                                    nullCheckTree->gtFlags |= GTF_ORDER_SIDEEFF;
                                                    nullCheckTree->gtFlags |= GTF_IND_NONFAULTING;

                                                    defRHS->gtFlags &= ~(GTF_EXCEPT | GTF_DONT_CSE);
                                                    defRHS->gtFlags |=
                                                        additionNode->gtFlags & (GTF_EXCEPT | GTF_DONT_CSE);

                                                    // Re-morph the statement.
                                                    fgMorphBlockStmt(compCurBB, curStmt DEBUGARG("optFoldNullCheck"));
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

//----------------------------------------------------------------
// optCanMoveNullCheckPastTree: Check if GT_NULLCHECK can be folded into a node that
//                              is after tree is execution order.
//
// Arguments:
//    tree           - The input GT_INDIR tree.
//    isInsideTry    - True if tree is inside try, false otherwise
//
// Return Value:
//    True if GT_NULLCHECK can be folded into a node that is after tree is execution order,
//    false otherwise.

bool Compiler::optCanMoveNullCheckPastTree(GenTree* tree, bool isInsideTry)
{
    bool result = true;
    if (isInsideTry)
    {
        // We disallow calls, exception sources, and all assignments.
        // Assignments to locals are disallowed inside try because
        // they may be live in the handler.
        if ((tree->gtFlags & GTF_SIDE_EFFECT) != 0)
        {
            result = false;
        }
    }
    else
    {
        // We disallow calls, exception sources, and assignments to
        // global memory.
        if (GTF_GLOBALLY_VISIBLE_SIDE_EFFECTS(tree->gtFlags))
        {
            result = false;
        }
    }
    return result;
}
