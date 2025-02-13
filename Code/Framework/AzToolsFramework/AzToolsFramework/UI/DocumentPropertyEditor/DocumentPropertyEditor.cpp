/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#include "DocumentPropertyEditor.h"

#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include <AzCore/DOM/DomUtils.h>
#include <AzFramework/DocumentPropertyEditor/PropertyEditorNodes.h>
#include <AzToolsFramework/UI/DocumentPropertyEditor/PropertyEditorToolsSystemInterface.h>

namespace AzToolsFramework
{
    // helper method used by both the view and row widgets to empty layouts
    static void DestroyLayoutContents(QLayout* layout)
    {
        while (auto layoutItem = layout->takeAt(0))
        {
            auto subWidget = layoutItem->widget();
            if (subWidget)
            {
                delete subWidget;
            }
            delete layoutItem;
        }
    }

    DPERowWidget::DPERowWidget(QWidget* parentWidget)
        : QWidget(parentWidget)
    {
        m_mainLayout = new QVBoxLayout(this);
        setLayout(m_mainLayout);
        m_columnLayout = new QHBoxLayout();
        m_mainLayout->addLayout(m_columnLayout);
        m_childRowLayout = new QVBoxLayout();
        m_mainLayout->addLayout(m_childRowLayout);
    }

    DPERowWidget::~DPERowWidget()
    {
        Clear();
    }

    void DPERowWidget::Clear()
    {
        // propertyHandlers own their widgets, so don't destroy them here. Set them free!
        for (auto propertyWidgetIter = m_widgetToPropertyHandler.begin(), endIter = m_widgetToPropertyHandler.end();
             propertyWidgetIter != endIter; ++propertyWidgetIter)
        {
            m_columnLayout->removeWidget(propertyWidgetIter->first);
        }
        m_widgetToPropertyHandler.clear();

        DestroyLayoutContents(m_columnLayout);
        DestroyLayoutContents(m_childRowLayout);
        m_domOrderedChildren.clear();
    }

    void DPERowWidget::AddChildFromDomValue(const AZ::Dom::Value& childValue, int domIndex)
    {
        auto childType = childValue.GetNodeName();

        // create a child widget from the given DOM value and add it to the correct layout
        QBoxLayout* layoutForWidget = nullptr;
        QWidget* addedWidget = nullptr;
        if (childType == AZ::Dpe::GetNodeName<AZ::Dpe::Nodes::Row>())
        {
            // if it's a row, recursively populate the children from the DOM array in the passed value
            auto newRow = new DPERowWidget(this);
            newRow->SetValueFromDom(childValue);
            addedWidget = newRow;
            layoutForWidget = m_childRowLayout;
        }
        else if (childType == AZ::Dpe::GetNodeName<AZ::Dpe::Nodes::Label>())
        {
            auto labelString = childValue[AZ::DocumentPropertyEditor::Nodes::Label::Value.GetName()].GetString();
            addedWidget = new QLabel(QString::fromUtf8(labelString.data(), static_cast<int>(labelString.size())), this);
            layoutForWidget = m_columnLayout;
        }
        else if (childType == AZ::Dpe::GetNodeName<AZ::Dpe::Nodes::PropertyEditor>())
        {
            auto dpeSystem = AZ::Interface<AzToolsFramework::PropertyEditorToolsSystemInterface>::Get();
            auto handlerId = dpeSystem->GetPropertyHandlerForNode(childValue);

            // if we found a valid handler, grab its widget and add it to our column layout
            if (handlerId)
            {
                // store, then reference the unique_ptr that will manage the handler's lifetime
                auto handler = dpeSystem->CreateHandlerInstance(handlerId);
                handler->SetValueFromDom(childValue);
                addedWidget = handler->GetWidget();
                m_widgetToPropertyHandler[addedWidget] = AZStd::move(handler);
                layoutForWidget = m_columnLayout;
            }
        }
        else
        {
            AZ_Assert(0, "unknown node type for DPE");
        }

        // insert this new widget into the dom order, even if it's an empty widget to ensure the order is correct
        auto insertIterator = m_domOrderedChildren.begin() + domIndex;
        insertIterator = m_domOrderedChildren.insert(insertIterator, addedWidget);

        // properly place the new widget in its given layout, correctly ordered by DOM index
        if (addedWidget && layoutForWidget)
        {
            // search subsequent dom entries for the first occurrence of a widget in the same layout,
            // then insert this new widget right before it. If there aren't any, then append this to the layout
            ++insertIterator;
            bool inserted = false;
            while (!inserted && insertIterator != m_domOrderedChildren.end())
            {
                const int insertIndex = layoutForWidget->indexOf(*insertIterator);
                if (insertIndex != -1)
                {
                    layoutForWidget->insertWidget(insertIndex, addedWidget);
                    inserted = true;
                }
            }
            if (!inserted)
            {
                layoutForWidget->addWidget(addedWidget);
            }
        }
    }

    void DPERowWidget::SetValueFromDom(const AZ::Dom::Value& domArray)
    {
        Clear();

        // populate all direct children of this row
        for (size_t arrayIndex = 0, numIndices = domArray.ArraySize(); arrayIndex < numIndices; ++arrayIndex)
        {
            auto& childValue = domArray[arrayIndex];
            AddChildFromDomValue(childValue, static_cast<int>(arrayIndex));
        }
    }

    void DPERowWidget::HandleOperationAtPath(const AZ::Dom::PatchOperation& domOperation, size_t pathIndex)
    {
        const auto& fullPath = domOperation.GetDestinationPath();
        auto pathEntry = fullPath[pathIndex];
        AZ_Assert(pathEntry.IsIndex() || pathEntry.IsEndOfArray(), "the direct children of a row must be referenced by index");

        // if we're on the last entry in the path, this row widget is the direct owner
        if (pathIndex == fullPath.Size() - 1)
        {
            int childIndex = 0;
            if (pathEntry.IsIndex())
            {
                // remove and replace operations must match an existing index. Add operations can be one past the current end.
                AZ_Assert(
                    (domOperation.GetType() == AZ::Dom::PatchOperation::Type::Add ? childIndex <= m_domOrderedChildren.size()
                                                                                  : childIndex < m_domOrderedChildren.size()),
                    "patch index is beyond the array bounds!");

                childIndex = static_cast<int>(pathEntry.GetIndex());
            }
            else if (domOperation.GetType() == AZ::Dom::PatchOperation::Type::Add)
            {
                childIndex = static_cast<int>(m_domOrderedChildren.size());
            }
            else // must be IsEndOfArray and a replace or remove, use the last existing index
            {
                childIndex = static_cast<int>(m_domOrderedChildren.size() - 1);
            }

            // if this is a remove or replace, remove the existing entry first,
            // then, if this is a replace or add, add the new entry
            if (domOperation.GetType() == AZ::Dom::PatchOperation::Type::Remove ||
                domOperation.GetType() == AZ::Dom::PatchOperation::Type::Replace)
            {
                const auto childIterator = m_domOrderedChildren.begin() + childIndex;
                delete *childIterator; // deleting the widget also automatically removes it from the layout
                m_domOrderedChildren.erase(childIterator);
            }

            if (domOperation.GetType() == AZ::Dom::PatchOperation::Type::Replace ||
                domOperation.GetType() == AZ::Dom::PatchOperation::Type::Add)
            {
                AddChildFromDomValue(domOperation.GetValue(), static_cast<int>(childIndex));
            }
        }
        else // not the direct owner of the entry to patch
        {
            // find the next widget in the path and delegate the operation to them
            const size_t childIndex = (pathEntry.IsIndex() ? pathEntry.GetIndex() : m_domOrderedChildren.size() - 1);
            AZ_Assert(childIndex <= m_domOrderedChildren.size(), "DPE: Patch failed to apply, invalid child index specified");
            if (childIndex > m_domOrderedChildren.size())
            {
                return;
            }
            QWidget* childWidget = m_domOrderedChildren[childIndex];
            if (m_childRowLayout->indexOf(childWidget) != -1)
            {
                // child is a DPERowWidget if it's in this layout, pass patch processing to it
                static_cast<DPERowWidget*>(childWidget)->HandleOperationAtPath(domOperation, pathIndex + 1);
            }
            else // child must be a label or a PropertyEditor
            {
                // pare down the path to this node, then look up and set the value from the DOM
                auto subPath = fullPath;
                for (size_t pathEntryIndex = fullPath.size() - 1; pathEntryIndex > pathIndex; --pathEntryIndex)
                {
                    subPath.Pop();
                }
                const auto valueAtSubPath = GetDPE()->GetAdapter()->GetContents()[subPath];
                m_widgetToPropertyHandler[childWidget]->SetValueFromDom(valueAtSubPath);
            }
        }
    }

    DocumentPropertyEditor* DPERowWidget::GetDPE()
    {
        DocumentPropertyEditor* theDPE = nullptr;
        QWidget* ancestorWidget = parentWidget();
        while (ancestorWidget && !theDPE)
        {
            theDPE = qobject_cast<DocumentPropertyEditor*>(ancestorWidget);
            ancestorWidget = ancestorWidget->parentWidget();
        }
        AZ_Assert(theDPE, "the top level widget in any DPE hierarchy must be the DocumentPropertyEditor itself!");
        return theDPE;
    }

    DocumentPropertyEditor::DocumentPropertyEditor(QWidget* parentWidget)
        : QFrame(parentWidget)
    {
        new QVBoxLayout(this);
    }

    DocumentPropertyEditor::~DocumentPropertyEditor()
    {
        DestroyLayoutContents(GetVerticalLayout());
    }

    void DocumentPropertyEditor::SetAdapter(AZ::DocumentPropertyEditor::DocumentAdapter* theAdapter)
    {
        m_adapter = theAdapter;
        m_resetHandler = AZ::DocumentPropertyEditor::DocumentAdapter::ResetEvent::Handler(
            [this]()
            {
                this->HandleReset();
            });
        m_adapter->ConnectResetHandler(m_resetHandler);

        m_changedHandler = AZ::DocumentPropertyEditor::DocumentAdapter::ChangedEvent::Handler(
            [this](const AZ::Dom::Patch& patch)
            {
                this->HandleDomChange(patch);
            });
        m_adapter->ConnectChangedHandler(m_changedHandler);

        // populate the view from the full adapter contents, just like a reset
        HandleReset();
    }

    QVBoxLayout* DocumentPropertyEditor::GetVerticalLayout()
    {
        return static_cast<QVBoxLayout*>(layout());
    }

    void DocumentPropertyEditor::AddRowFromValue(const AZ::Dom::Value& domValue, int rowIndex)
    {
        auto newRow = new DPERowWidget(this);
        newRow->SetValueFromDom(domValue);
        GetVerticalLayout()->insertWidget(rowIndex, newRow);
    }

    void DocumentPropertyEditor::HandleReset()
    {
        // clear any pre-existing DPERowWidgets
        DestroyLayoutContents(GetVerticalLayout());

        auto topContents = m_adapter->GetContents();

        for (size_t arrayIndex = 0, numIndices = topContents.ArraySize(); arrayIndex < numIndices; ++arrayIndex)
        {
            auto& rowValue = topContents[arrayIndex];
            auto domName = rowValue.GetNodeName().GetStringView();
            const bool isRow = (domName == AZ::DocumentPropertyEditor::Nodes::Row::Name);
            AZ_Assert(isRow, "adapters must only have rows as direct children!");

            if (isRow)
            {
                AddRowFromValue(rowValue, static_cast<int>(arrayIndex));
            }
        }
    }
    void DocumentPropertyEditor::HandleDomChange(const AZ::Dom::Patch& patch)
    {
        auto* rowLayout = GetVerticalLayout();

        for (auto operationIterator = patch.begin(), endIterator = patch.end(); operationIterator != endIterator; ++operationIterator)
        {
            const auto& patchPath = operationIterator->GetDestinationPath();
            auto firstAddressEntry = patchPath[0];

            AZ_Assert(
                firstAddressEntry.IsIndex() || firstAddressEntry.IsEndOfArray(),
                "first entry in a DPE patch must be the index of the first row");
            auto rowIndex = (firstAddressEntry.IsIndex() ? firstAddressEntry.GetIndex() : rowLayout->count());
            AZ_Assert(
                rowIndex < rowLayout->count() ||
                    (rowIndex <= rowLayout->count() && operationIterator->GetType() == AZ::Dom::PatchOperation::Type::Add),
                "received a patch for a row that doesn't exist");

            // if the patch points at our root, this operation is for the top level layout
            if (patchPath.IsEmpty())
            {
                if (operationIterator->GetType() == AZ::Dom::PatchOperation::Type::Add)
                {
                    AddRowFromValue(operationIterator->GetValue(), static_cast<int>(rowIndex));
                }
                else
                {
                    auto rowWidget =
                        static_cast<DPERowWidget*>(GetVerticalLayout()->itemAt(static_cast<int>(firstAddressEntry.GetIndex()))->widget());
                    if (operationIterator->GetType() == AZ::Dom::PatchOperation::Type::Replace)
                    {
                        rowWidget->SetValueFromDom(operationIterator->GetValue());
                    }
                    else if (operationIterator->GetType() == AZ::Dom::PatchOperation::Type::Remove)
                    {
                        rowLayout->removeWidget(rowWidget);
                        delete rowWidget;
                    }
                }
            }
            else
            {
                // delegate the action th the rowWidget, which will, in turn, delegate to the next row in the path, if available
                auto rowWidget =
                    static_cast<DPERowWidget*>(GetVerticalLayout()->itemAt(static_cast<int>(firstAddressEntry.GetIndex()))->widget());

                constexpr size_t pathDepth = 1; // top level has been handled, start the next operation at path depth 1
                rowWidget->HandleOperationAtPath(*operationIterator, pathDepth);
            }
        }
    }

} // namespace AzToolsFramework
