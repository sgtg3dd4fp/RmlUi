/*
 * This source file is part of RmlUi, the HTML/CSS Interface Middleware
 *
 * For the latest information, see http://github.com/mikke89/RmlUi
 *
 * Copyright (c) 2008-2010 CodePoint Ltd, Shift Technology Ltd
 * Copyright (c) 2019 The RmlUi Team, and contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "precompiled.h"
#include "../../Include/RmlUi/Core.h"
#include "EventDispatcher.h"
#include "EventIterators.h"
#include "PluginRegistry.h"
#include "StreamFile.h"
#include "../../Include/RmlUi/Core/StreamMemory.h"
#include <algorithm>
#include <iterator>

namespace Rml {
namespace Core {

const float DOUBLE_CLICK_TIME = 0.5f;

Context::Context(const String& name) : name(name), dimensions(0, 0), density_independent_pixel_ratio(1.0f), mouse_position(0, 0), clip_origin(-1, -1), clip_dimensions(-1, -1), view_state()
{
	instancer = nullptr;

	// Initialise this to nullptr; this will be set in Rml::Core::CreateContext().
	render_interface = nullptr;

	root = Factory::InstanceElement(nullptr, "*", "#root", XMLAttributes());
	root->SetId(name);
	root->SetOffset(Vector2f(0, 0), nullptr);
	root->SetProperty(PropertyId::ZIndex, Property(0, Property::NUMBER));

	cursor_proxy = Factory::InstanceElement(nullptr, "body", "body", XMLAttributes());
	ElementDocument* cursor_proxy_document = dynamic_cast< ElementDocument* >(cursor_proxy.get());
	if (cursor_proxy_document)
		cursor_proxy_document->context = this;
	else
		cursor_proxy.reset();
		

	document_focus_history.push_back(root.get());
	focus = root.get();

	enable_cursor = true;

	focus = nullptr;
	hover = nullptr;
	active = nullptr;
	drag = nullptr;

	drag_started = false;
	drag_verbose = false;
	drag_clone = nullptr;
	drag_hover = nullptr;

	last_click_element = nullptr;
	last_click_time = 0;
}

Context::~Context()
{
	PluginRegistry::NotifyContextDestroy(this);

	UnloadAllDocuments();

	ReleaseUnloadedDocuments();

	cursor_proxy.reset();

	root.reset();

	instancer.reset();

	if (render_interface)
		render_interface->RemoveReference();
}

// Returns the name of the context.
const String& Context::GetName() const
{
	return name;
}

// Changes the dimensions of the screen.
void Context::SetDimensions(const Vector2i& _dimensions)
{
	if (dimensions != _dimensions)
	{
		dimensions = _dimensions;
		root->SetBox(Box(Vector2f((float) dimensions.x, (float) dimensions.y)));
		root->DirtyLayout();

		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document != nullptr)
			{
				document->DirtyLayout();
				document->DirtyPosition();
				document->DispatchEvent(EventId::Resize, Dictionary());
			}
		}
		
		clip_dimensions = dimensions;


		// TODO: Ensure the user calls ProcessProjectionChange() before
		// the next rendering phase.
	}
}

// Returns the dimensions of the screen.
const Vector2i& Context::GetDimensions() const
{
	return dimensions;
}


// Returns the current state of the view.
const ViewState& Context::GetViewState() const noexcept
{
	return view_state;
}

void Context::SetDensityIndependentPixelRatio(float _density_independent_pixel_ratio)
{
	if (density_independent_pixel_ratio != _density_independent_pixel_ratio)
	{
		density_independent_pixel_ratio = _density_independent_pixel_ratio;

		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
			if (document != nullptr)
			{
				document->DirtyDpProperties();
			}
		}
	}
}

float Context::GetDensityIndependentPixelRatio() const
{
	return density_independent_pixel_ratio;
}

// Updates all elements in the element tree.
bool Context::Update()
{
	root->Update(density_independent_pixel_ratio);

	for (int i = 0; i < root->GetNumChildren(); ++i)
		if (auto doc = root->GetChild(i)->GetOwnerDocument())
		{
			doc->UpdateLayout();
			doc->UpdatePosition();
		}

	// Release any documents that were unloaded during the update.
	ReleaseUnloadedDocuments();

	return true;
}

// Renders all visible elements in the element tree.
bool Context::Render()
{
	RenderInterface* render_interface = GetRenderInterface();
	if (render_interface == nullptr)
		return false;

	render_interface->context = this;
	ElementUtilities::ApplyActiveClipRegion(this, render_interface);

	root->Render();

	ElementUtilities::SetClippingRegion(nullptr, this);

	// Render the cursor proxy so any elements attached the cursor will be rendered below the cursor.
	if (cursor_proxy)
	{
		static_cast<ElementDocument&>(*cursor_proxy).UpdateDocument();
		cursor_proxy->SetOffset(Vector2f((float)Math::Clamp(mouse_position.x, 0, dimensions.x),
			(float)Math::Clamp(mouse_position.y, 0, dimensions.y)),
			nullptr);
		cursor_proxy->Render();
	}

	render_interface->context = nullptr;

	return true;
}

// Creates a new, empty document and places it into this context.
ElementDocument* Context::CreateDocument(const String& tag)
{
	ElementPtr element = Factory::InstanceElement(nullptr, tag, "body", XMLAttributes());
	if (!element)
	{
		Log::Message(Log::LT_ERROR, "Failed to instance document on tag '%s', instancer returned nullptr.", tag.c_str());
		return nullptr;
	}

	ElementDocument* document = dynamic_cast< ElementDocument* >(element.get());
	if (!document)
	{
		Log::Message(Log::LT_ERROR, "Failed to instance document on tag '%s', Found type '%s', was expecting derivative of ElementDocument.", tag.c_str(), typeid(element).name());
		return nullptr;
	}

	document->context = this;
	root->AppendChild(std::move(element));

	PluginRegistry::NotifyDocumentLoad(document);

	return document;
}

// Load a document into the context.
ElementDocument* Context::LoadDocument(const String& document_path)
{	
	StreamFile* stream = new StreamFile();
	if (!stream->Open(document_path))
	{
		stream->RemoveReference();
		return nullptr;
	}

	ElementDocument* document = LoadDocument(stream);

	stream->RemoveReference();

	return document;
}

// Load a document into the context.
ElementDocument* Context::LoadDocument(Stream* stream)
{
	PluginRegistry::NotifyDocumentOpen(this, stream->GetSourceURL().GetURL());

	ElementPtr element = Factory::InstanceDocumentStream(this, stream);
	if (!element)
		return nullptr;

	ElementDocument* document = static_cast<ElementDocument*>(element.get());
	
	root->AppendChild(std::move(element));

	ElementUtilities::BindEventAttributes(document);

	// The 'load' event is fired before updating the document, because the user might
	// need to initalize things before running an update. The drawback is that computed
	// values and layouting are not performed yet, resulting in default values when
	// querying such information in the event handler.
	PluginRegistry::NotifyDocumentLoad(document);
	document->DispatchEvent(EventId::Load, Dictionary());

	document->UpdateDocument();

	return document;
}

// Load a document into the context.
ElementDocument* Context::LoadDocumentFromMemory(const String& string)
{
	// Open the stream based on the string contents.
	StreamMemory* stream = new StreamMemory((byte*)string.c_str(), string.size());
	stream->SetSourceURL("[document from memory]");

	// Load the document from the stream.
	ElementDocument* document = LoadDocument(stream);

	stream->RemoveReference();

	return document;
}

// Unload the given document
void Context::UnloadDocument(ElementDocument* _document)
{
	// Has this document already been unloaded?
	for (size_t i = 0; i < unloaded_documents.size(); ++i)
	{
		if (unloaded_documents[i].get() == _document)
			return;
	}

	// Add a reference, to ensure the document isn't released
	// while we're closing it.
	ElementDocument* document = _document;

	if (document->GetParentNode() == root.get())
	{
		// Dispatch the unload notifications.
		document->DispatchEvent(EventId::Unload, Dictionary());
		PluginRegistry::NotifyDocumentUnload(document);

		// Move document to a temporary location to be released later.
		unloaded_documents.push_back( root->RemoveChild(document) );
	}

	// Remove the item from the focus history.
	ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), document);
	if (itr != document_focus_history.end())
		document_focus_history.erase(itr);

	// Focus to the previous document if the old document is the current focus.
	if (focus && focus->GetOwnerDocument() == document)
	{
		focus = nullptr;
		document_focus_history.back()->GetFocusLeafNode()->Focus();
	}

	// Clear the active element if the old document is the active element.
	if (active && active->GetOwnerDocument() == document)
	{
		active = nullptr;
	}

	// Clear other pointers to elements whose owner was deleted
	if (drag && drag->GetOwnerDocument() == document)
	{
		drag = nullptr;
	}

	if (drag_hover && drag_hover->GetOwnerDocument() == document)
	{
		drag_hover = nullptr;
	}

	// Rebuild the hover state.
	UpdateHoverChain(Dictionary(), Dictionary(), mouse_position);
}

// Unload all the currently loaded documents
void Context::UnloadAllDocuments()
{
	// Unload all children.
	while (root->GetNumChildren(true) > 0)
		UnloadDocument(root->GetChild(0)->GetOwnerDocument());

	// Also need to clear containers that keep ElementReference pointers to elements belonging to removed documents,
	// essentially preventing them from being released in correct order (before context destroys render interface,
	// which causes access violation for elements that try to release geometry after context is released)
	// I don't bother checking what's in chain because we unload all documents, so there shouldn't be any element
	// that remains here (could check for element->owner == nullptr, but that's probably guaranteed)
	active_chain.clear();
	hover_chain.clear();
	drag_hover_chain.clear();
}

// Enables or disables the mouse cursor.
void Context::EnableMouseCursor(bool enable)
{
	// The cursor is set to an invalid name so that it is forced to update in the next update loop.
	cursor_name = ":reset:";
	enable_cursor = enable;
}

// Returns the first document found in the root with the given id.
ElementDocument* Context::GetDocument(const String& id)
{
	for (int i = 0; i < root->GetNumChildren(); i++)
	{
		ElementDocument* document = root->GetChild(i)->GetOwnerDocument();
		if (document == nullptr)
			continue;

		if (document->GetId() == id)
			return document;
	}

	return nullptr;
}

// Returns a document in the context by index.
ElementDocument* Context::GetDocument(int index)
{
	Element* element = root->GetChild(index);
	if (element == nullptr)
		return nullptr;

	return element->GetOwnerDocument();
}

// Returns the number of documents in the context.
int Context::GetNumDocuments() const
{
	return root->GetNumChildren();
}

// Returns the hover element.
Element* Context::GetHoverElement()
{
	return hover;
}

// Returns the focus element.
Element* Context::GetFocusElement()
{
	return focus;
}

// Returns the root element.
Element* Context::GetRootElement()
{
	return root.get();
}

// Brings the document to the front of the document stack.
void Context::PullDocumentToFront(ElementDocument* document)
{
	if (document != root->GetLastChild())
	{
		// Calling RemoveChild() / AppendChild() would be cleaner, but that dirties the document's layout
		// unnecessarily, so we'll go under the hood here.
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin() + root->GetNumChildren(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

// Sends the document to the back of the document stack.
void Context::PushDocumentToBack(ElementDocument* document)
{
	if (document != root->GetFirstChild())
	{
		// See PullDocumentToFront().
		for (int i = 0; i < root->GetNumChildren(); ++i)
		{
			if (root->GetChild(i) == document)
			{
				ElementPtr element = std::move(root->children[i]);
				root->children.erase(root->children.begin() + i);
				root->children.insert(root->children.begin(), std::move(element));

				root->DirtyStackingContext();
			}
		}
	}
}

// Adds an event listener to the root element.
void Context::AddEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->AddEventListener(event, listener, in_capture_phase);
}

// Removes an event listener from the root element.
void Context::RemoveEventListener(const String& event, EventListener* listener, bool in_capture_phase)
{
	root->RemoveEventListener(event, listener, in_capture_phase);
}

// Sends a key down event into RmlUi.
bool Context::ProcessKeyDown(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	if (focus)
		return focus->DispatchEvent(EventId::Keydown, parameters);
	else
		return root->DispatchEvent(EventId::Keydown, parameters);
}

// Sends a key up event into RmlUi.
bool Context::ProcessKeyUp(Input::KeyIdentifier key_identifier, int key_modifier_state)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	GenerateKeyEventParameters(parameters, key_identifier);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	if (focus)
		return focus->DispatchEvent(EventId::Keyup, parameters);
	else
		return root->DispatchEvent(EventId::Keyup, parameters);
}

// Sends a single character of text as text input into RmlUi.
bool Context::ProcessTextInput(word character)
{
	// Generate the parameters for the key event.
	Dictionary parameters;
	parameters["data"] = character;

	if (focus)
		return focus->DispatchEvent(EventId::Textinput, parameters);
	else
		return root->DispatchEvent(EventId::Textinput, parameters);
}

// Sends a string of text as text input into RmlUi.
bool Context::ProcessTextInput(const String& string)
{
	bool consumed = true;

	for (size_t i = 0; i < string.size(); ++i)
	{
		// Generate the parameters for the key event.
		Dictionary parameters;
		parameters["data"] = string[i];

		if (focus)
			consumed = focus->DispatchEvent(EventId::Textinput, parameters) && consumed;
		else
			consumed = root->DispatchEvent(EventId::Textinput, parameters) && consumed;
	}

	return consumed;
}

// Sends a mouse movement event into RmlUi.
void Context::ProcessMouseMove(int x, int y, int key_modifier_state)
{
	// Check whether the mouse moved since the last event came through.
	Vector2i old_mouse_position = mouse_position;
	bool mouse_moved = (x != mouse_position.x) || (y != mouse_position.y);
	if (mouse_moved)
	{
		mouse_position.x = x;
		mouse_position.y = y;
	}

	// Generate the parameters for the mouse events (there could be a few!).
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, -1);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	Dictionary drag_parameters;
	GenerateMouseEventParameters(drag_parameters);
	GenerateDragEventParameters(drag_parameters);
	GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

	// Update the current hover chain. This will send all necessary 'onmouseout', 'onmouseover', 'ondragout' and
	// 'ondragover' messages.
	UpdateHoverChain(parameters, drag_parameters, old_mouse_position);

	// Dispatch any 'onmousemove' events.
	if (mouse_moved)
	{
		if (hover)
		{
			hover->DispatchEvent(EventId::Mousemove, parameters);

			if (drag_hover &&
				drag_verbose)
				drag_hover->DispatchEvent(EventId::Dragmove, drag_parameters);
		}
	}
}
	
static Element* FindFocusElement(Element* element)
{
	ElementDocument* owner_document = element->GetOwnerDocument();
	if (!owner_document || owner_document->GetComputedValues().focus == Style::Focus::None)
		return nullptr;
	
	while (element && element->GetComputedValues().focus == Style::Focus::None)
	{
		element = element->GetParentNode();
	}
	
	return element;
}

// Sends a mouse-button down event into RmlUi.
void Context::ProcessMouseButtonDown(int button_index, int key_modifier_state)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, button_index);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	if (button_index == 0)
	{
		Element* new_focus = hover;
		
		// Set the currently hovered element to focus if it isn't already the focus.
		if (hover)
		{
			new_focus = FindFocusElement(hover);
			if (new_focus && new_focus != focus)
			{
				if (!new_focus->Focus())
					return;
			}
		}

		// Save the just-pressed-on element as the pressed element.
		active = new_focus;

		bool propogate = true;
		
		// Call 'onmousedown' on every item in the hover chain, and copy the hover chain to the active chain.
		if (hover)
			propogate = hover->DispatchEvent(EventId::Mousedown, parameters);

		if (propogate)
		{
			// Check for a double-click on an element; if one has occured, we send the 'dblclick' event to the hover
			// element. If not, we'll start a timer to catch the next one.
			double click_time = GetSystemInterface()->GetElapsedTime();
			if (active == last_click_element &&
				float(click_time - last_click_time) < DOUBLE_CLICK_TIME)
			{
				if (hover)
					propogate = hover->DispatchEvent(EventId::Dblclick, parameters);

				last_click_element = nullptr;
				last_click_time = 0;
			}
			else
			{
				last_click_element = active;
				last_click_time = click_time;
			
			}
		}

		for (ElementSet::iterator itr = hover_chain.begin(); itr != hover_chain.end(); ++itr)
			active_chain.push_back((*itr));

		if (propogate)
		{
			// Traverse down the hierarchy of the newly focussed element (if any), and see if we can begin dragging it.
			drag_started = false;
			drag = hover;
			while (drag)
			{
				Style::Drag drag_style = drag->GetComputedValues().drag;
				switch (drag_style)
				{
				case Style::Drag::None:		drag = drag->GetParentNode(); continue;
				case Style::Drag::Block:	drag = nullptr; continue;
				default: drag_verbose = (drag_style == Style::Drag::DragDrop || drag_style == Style::Drag::Clone);
				}

				break;
			}
		}
	}
	else
	{
		// Not the primary mouse button, so we're not doing any special processing.
		if (hover)
			hover->DispatchEvent(EventId::Mousedown, parameters);
	}
}

// Sends a mouse-button up event into RmlUi.
void Context::ProcessMouseButtonUp(int button_index, int key_modifier_state)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, button_index);
	GenerateKeyModifierEventParameters(parameters, key_modifier_state);

	// Process primary click.
	if (button_index == 0)
	{
		// The elements in the new hover chain have the 'onmouseup' event called on them.
		if (hover)
			hover->DispatchEvent(EventId::Mouseup, parameters);

		// If the active element (the one that was being hovered over when the mouse button was pressed) is still being
		// hovered over, we click it.
		if (hover && active && active == FindFocusElement(hover))
		{
			active->DispatchEvent(EventId::Click, parameters);
		}

		// Unset the 'active' pseudo-class on all the elements in the active chain; because they may not necessarily
		// have had 'onmouseup' called on them, we can't guarantee this has happened already.
		auto func = PseudoClassFunctor("active", false);
		std::for_each(active_chain.begin(), active_chain.end(), func);
		active_chain.clear();

		if (drag)
		{
			if (drag_started)
			{
				Dictionary drag_parameters;
				GenerateMouseEventParameters(drag_parameters);
				GenerateDragEventParameters(drag_parameters);
				GenerateKeyModifierEventParameters(drag_parameters, key_modifier_state);

				if (drag_hover)
				{
					if (drag_verbose)
					{
						drag_hover->DispatchEvent(EventId::Dragdrop, drag_parameters);
						drag_hover->DispatchEvent(EventId::Dragout, drag_parameters);
					}
				}

				drag->DispatchEvent(EventId::Dragend, drag_parameters);

				ReleaseDragClone();
			}

			drag = nullptr;
			drag_hover = nullptr;
			drag_hover_chain.clear();

			// We may have changes under our mouse, this ensures that the hover chain is properly updated
			ProcessMouseMove(mouse_position.x, mouse_position.y, key_modifier_state);
		}
	}
	else
	{
		// Not the left mouse button, so we're not doing any special processing.
		if (hover)
			hover->DispatchEvent(EventId::Mouseup, parameters);
	}
}

// Sends a mouse-wheel movement event into RmlUi.
bool Context::ProcessMouseWheel(float wheel_delta, int key_modifier_state)
{
	if (hover)
	{
		Dictionary scroll_parameters;
		GenerateKeyModifierEventParameters(scroll_parameters, key_modifier_state);
		scroll_parameters["wheel_delta"] = wheel_delta;

		return hover->DispatchEvent(EventId::Mousescroll, scroll_parameters);
	}

	return true;
}

// Notifies RmlUi of a change in the projection matrix.
void Context::ProcessProjectionChange(const Matrix4f &projection)
{
	view_state.SetProjection(&projection);
}

// Notifies RmlUi of a change in the view matrix.
void Context::ProcessViewChange(const Matrix4f &view)
{
	view_state.SetView(&view);
}

// Gets the context's render interface.
RenderInterface* Context::GetRenderInterface() const
{
	return render_interface;
}
	
// Gets the current clipping region for the render traversal
bool Context::GetActiveClipRegion(Vector2i& origin, Vector2i& dimensions) const
{
	if (clip_dimensions.x < 0 || clip_dimensions.y < 0)
		return false;
	
	origin = clip_origin;
	dimensions = clip_dimensions;
	
	return true;
}
	
// Sets the current clipping region for the render traversal
void Context::SetActiveClipRegion(const Vector2i& origin, const Vector2i& dimensions)
{
	clip_origin = origin;
	clip_dimensions = dimensions;
}

// Sets the instancer to use for releasing this object.
void Context::SetInstancer(SharedPtr<ContextInstancer> _instancer)
{
	RMLUI_ASSERT(instancer == nullptr);
	instancer = std::move(_instancer);
}

// Internal callback for when an element is removed from the hierarchy.
void Context::OnElementRemove(Element* element)
{
	ElementSet::iterator i = hover_chain.find(element);
	if (i == hover_chain.end())
		return;

	ElementSet old_hover_chain = hover_chain;
	hover_chain.erase(i);

	Element* hover_element = element;
	while (hover_element != nullptr)
	{
		Element* next_hover_element = nullptr;

		// Look for a child on this element's children that is also hovered.
		for (int j = 0; j < hover_element->GetNumChildren(true); ++j)
		{
			// Is this child also in the hover chain?
			Element* hover_child_element = hover_element->GetChild(j);
			ElementSet::iterator k = hover_chain.find(hover_child_element);
			if (k != hover_chain.end())
			{
				next_hover_element = hover_child_element;
				hover_chain.erase(k);

				break;
			}
		}

		hover_element = next_hover_element;
	}

	Dictionary parameters;
	GenerateMouseEventParameters(parameters, -1);
	SendEvents(old_hover_chain, hover_chain, EventId::Mouseout, parameters);
}

// Internal callback for when a new element gains focus
bool Context::OnFocusChange(Element* new_focus)
{
	ElementSet old_chain;
	ElementSet new_chain;

	Element* old_focus = focus;
	ElementDocument* old_document = old_focus ? old_focus->GetOwnerDocument() : nullptr;
	ElementDocument* new_document = new_focus->GetOwnerDocument();

	// If the current focus is modal and the new focus is not modal, deny the request
	if (old_document && old_document->IsModal() && (!new_document || !new_document->GetOwnerDocument()->IsModal()))
		return false;

	// Build the old chains
	Element* element = old_focus;
	while (element)
	{
		old_chain.insert(element);
		element = element->GetParentNode();
	}

	// Build the new chain
	element = new_focus;
	while (element)
	{
		new_chain.insert(element);
		element = element->GetParentNode();
	}

	Dictionary parameters;

	// Send out blur/focus events.
	SendEvents(old_chain, new_chain, EventId::Blur, parameters);
	SendEvents(new_chain, old_chain, EventId::Focus, parameters);

	focus = new_focus;

	// Raise the element's document to the front, if desired.
	ElementDocument* document = focus->GetOwnerDocument();
	if (document != nullptr)
	{
		Style::ZIndex z_index_property = document->GetComputedValues().z_index;
		if (z_index_property.type == Style::ZIndex::Auto)
			document->PullToFront();
	}

	// Update the focus history
	if (old_document != new_document)
	{
		// If documents have changed, add the new document to the end of the history
		ElementList::iterator itr = std::find(document_focus_history.begin(), document_focus_history.end(), new_document);
		if (itr != document_focus_history.end())
			document_focus_history.erase(itr);

		if (new_document != nullptr)
			document_focus_history.push_back(new_document);
	}

	return true;
}

// Generates an event for faking clicks on an element.
void Context::GenerateClickEvent(Element* element)
{
	Dictionary parameters;
	GenerateMouseEventParameters(parameters, 0);

	element->DispatchEvent(EventId::Click, parameters);
}

// Updates the current hover elements, sending required events.
void Context::UpdateHoverChain(const Dictionary& parameters, const Dictionary& drag_parameters, const Vector2i& old_mouse_position)
{
	Vector2f position((float) mouse_position.x, (float) mouse_position.y);

	// Send out drag events.
	if (drag)
	{
		if (mouse_position != old_mouse_position)
		{
			if (!drag_started)
			{
				Dictionary drag_start_parameters = drag_parameters;
				drag_start_parameters["mouse_x"] = old_mouse_position.x;
				drag_start_parameters["mouse_y"] = old_mouse_position.y;
				drag->DispatchEvent(EventId::Dragstart, drag_start_parameters);
				drag_started = true;

				if (drag->GetComputedValues().drag == Style::Drag::Clone)
				{
					// Clone the element and attach it to the mouse cursor.
					CreateDragClone(drag);
				}
			}

			drag->DispatchEvent(EventId::Drag, drag_parameters);
		}
	}

	hover = GetElementAtPoint(position);

	if(enable_cursor)
	{
		String new_cursor_name;

		if(drag)
			new_cursor_name = drag->GetComputedValues().cursor;
		else if (hover)
			new_cursor_name = hover->GetComputedValues().cursor;

		if(new_cursor_name != cursor_name)
		{
			GetSystemInterface()->SetMouseCursor(new_cursor_name);
			cursor_name = new_cursor_name;
		}
	}

	// Build the new hover chain.
	ElementSet new_hover_chain;
	Element* element = hover;
	while (element != nullptr)
	{
		new_hover_chain.insert(element);
		element = element->GetParentNode();
	}

	// Send mouseout / mouseover events.
	SendEvents(hover_chain, new_hover_chain, EventId::Mouseout, parameters);
	SendEvents(new_hover_chain, hover_chain, EventId::Mouseover, parameters);

	// Send out drag events.
	if (drag)
	{
		drag_hover = GetElementAtPoint(position, drag);

		ElementSet new_drag_hover_chain;
		element = drag_hover;
		while (element != nullptr)
		{
			new_drag_hover_chain.insert(element);
			element = element->GetParentNode();
		}

		if (drag_started &&
			drag_verbose)
		{
			// Send out ondragover and ondragout events as appropriate.
			SendEvents(drag_hover_chain, new_drag_hover_chain, EventId::Dragout, drag_parameters);
			SendEvents(new_drag_hover_chain, drag_hover_chain, EventId::Dragover, drag_parameters);
		}

		drag_hover_chain.swap(new_drag_hover_chain);
	}

	// Swap the new chain in.
	hover_chain.swap(new_hover_chain);
}

// Returns the youngest descendent of the given element which is under the given point in screen coodinates.
Element* Context::GetElementAtPoint(const Vector2f& point, const Element* ignore_element, Element* element)
{
	if (element == nullptr)
	{
		if (ignore_element == root.get())
			return nullptr;

		element = root.get();
	}

	// Check if any documents have modal focus; if so, only check down than document.
	if (element == root.get())
	{
		if (focus)
		{
			ElementDocument* focus_document = focus->GetOwnerDocument();
			if (focus_document != nullptr &&
				focus_document->IsModal())
			{
				element = focus_document;
			}
		}
	}


	// Check any elements within our stacking context. We want to return the lowest-down element
	// that is under the cursor.
	if (element->local_stacking_context)
	{
		if (element->stacking_context_dirty)
			element->BuildLocalStackingContext();

		for (int i = (int) element->stacking_context.size() - 1; i >= 0; --i)
		{
			if (ignore_element != nullptr)
			{
				Element* element_hierarchy = element->stacking_context[i];
				while (element_hierarchy != nullptr)
				{
					if (element_hierarchy == ignore_element)
						break;

					element_hierarchy = element_hierarchy->GetParentNode();
				}

				if (element_hierarchy != nullptr)
					continue;
			}

			Element* child_element = GetElementAtPoint(point, ignore_element, element->stacking_context[i]);
			if (child_element != nullptr)
				return child_element;
		}
	}

	// Ignore elements whose pointer events are disabled
	if (element->GetComputedValues().pointer_events == Style::PointerEvents::None)
		return nullptr;

	Vector2f projected_point = element->Project(point);

	// Check if the point is actually within this element.
	bool within_element = element->IsPointWithinElement(projected_point);
	if (within_element)
	{
		Vector2i clip_origin, clip_dimensions;
		if (ElementUtilities::GetClippingRegion(clip_origin, clip_dimensions, element))
		{
			within_element = projected_point.x >= clip_origin.x &&
							 projected_point.y >= clip_origin.y &&
							 projected_point.x <= (clip_origin.x + clip_dimensions.x) &&
							 projected_point.y <= (clip_origin.y + clip_dimensions.y);
		}
	}

	if (within_element)
		return element;

	return nullptr;
}

// Creates the drag clone from the given element.
void Context::CreateDragClone(Element* element)
{
	if (!cursor_proxy)
	{
		Log::Message(Log::LT_ERROR, "Unable to create drag clone, no cursor proxy document.");
		return;
	}

	ReleaseDragClone();

	// Instance the drag clone.
	ElementPtr element_drag_clone = element->Clone();
	if (!element_drag_clone)
	{
		Log::Message(Log::LT_ERROR, "Unable to duplicate drag clone.");
		return;
	}

	drag_clone = element_drag_clone.get();

	// Append the clone to the cursor proxy element.
	cursor_proxy->AppendChild(std::move(element_drag_clone));

	// Set the style sheet on the cursor proxy.
	static_cast<ElementDocument&>(*cursor_proxy).SetStyleSheet(element->GetStyleSheet());

	// Set all the required properties and pseudo-classes on the clone.
	drag_clone->SetPseudoClass("drag", true);
	drag_clone->SetProperty(PropertyId::Position, Property(Style::Position::Absolute));
	drag_clone->SetProperty(PropertyId::Left, Property(element->GetAbsoluteLeft() - element->GetBox().GetEdge(Box::MARGIN, Box::LEFT) - mouse_position.x, Property::PX));
	drag_clone->SetProperty(PropertyId::Top, Property(element->GetAbsoluteTop() - element->GetBox().GetEdge(Box::MARGIN, Box::TOP) - mouse_position.y, Property::PX));
}

// Releases the drag clone, if one exists.
void Context::ReleaseDragClone()
{
	if (drag_clone)
	{
		cursor_proxy->RemoveChild(drag_clone);
		drag_clone = nullptr;
	}
}

// Builds the parameters for a generic key event.
void Context::GenerateKeyEventParameters(Dictionary& parameters, Input::KeyIdentifier key_identifier)
{
	parameters["key_identifier"] = (int)key_identifier;
}

// Builds the parameters for a generic mouse event.
void Context::GenerateMouseEventParameters(Dictionary& parameters, int button_index)
{
	parameters.reserve(3);
	parameters["mouse_x"] = mouse_position.x;
	parameters["mouse_y"] = mouse_position.y;
	if (button_index >= 0)
		parameters["button"] = button_index;
}

// Builds the parameters for the key modifier state.
void Context::GenerateKeyModifierEventParameters(Dictionary& parameters, int key_modifier_state)
{
	static String property_names[] = {
		"ctrl_key",
		"shift_key",
		"alt_key",
		"meta_key",
		"caps_lock_key",
		"num_lock_key",
		"scroll_lock_key"
	};

	for (int i = 0; i < 7; i++)
		parameters[property_names[i]] = (int)((key_modifier_state & (1 << i)) > 0);
}

// Builds the parameters for a drag event.
void Context::GenerateDragEventParameters(Dictionary& parameters)
{	
	parameters["drag_element"] = (void*)drag;
}

// Releases all unloaded documents pending destruction.
void Context::ReleaseUnloadedDocuments()
{
	if (!unloaded_documents.empty())
	{
		OwnedElementList documents = std::move(unloaded_documents);
		unloaded_documents.clear();

		// Clear the deleted list.
		for (size_t i = 0; i < documents.size(); ++i)
			documents[i]->GetEventDispatcher()->DetachAllEvents();
		documents.clear();
	}
}

// Sends the specified event to all elements in new_items that don't appear in old_items.
void Context::SendEvents(const ElementSet& old_items, const ElementSet& new_items, EventId id, const Dictionary& parameters)
{
	ElementList elements;
	std::set_difference(old_items.begin(), old_items.end(), new_items.begin(), new_items.end(), std::back_inserter(elements));
	RmlEventFunctor func(id, parameters);
	std::for_each(elements.begin(), elements.end(), func);
}

void Context::Release()
{
	if (instancer)
	{
		instancer->ReleaseContext(this);
	}
}

}
}
