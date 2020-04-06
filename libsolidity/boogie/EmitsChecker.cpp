#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <libsolidity/boogie/ASTBoogieUtils.h>
#include <libsolidity/boogie/EmitsChecker.h>

using namespace dev;
using namespace dev::solidity;
using namespace std;

bool EmitsChecker::collectEmitsSpecs(FunctionDefinition const* fn, list<EmitsSpec>& specs)
{
	set<string> specified;
	for (auto docTag: fn->annotation().docTags)
	{
		if (docTag.first == "notice" && boost::starts_with(docTag.second.content, ASTBoogieUtils::DOCTAG_EMITS))
		{
			string eventName = docTag.second.content.substr(ASTBoogieUtils::DOCTAG_EMITS.length() + 1);
			boost::trim(eventName);

			if (specified.find(eventName) != specified.end())
			{
				m_context.reportWarning(fn, "Event '" + eventName + "' already specified.");
				continue;
			}

			specs.push_back(EmitsSpec{ {}, false});
			for (auto cd: m_allFunctions[fn]->annotation().linearizedBaseContracts)
				for (auto ev: ASTNode::filteredNodes<EventDefinition const>(cd->subNodes()))
					if (ev->name() == eventName)
						specs.back().events.insert(ev);
			if (specs.back().events.empty())
			{
				m_context.reportError(fn, "No event found with name '" + eventName + "'.");
				return false;
			}
			specified.insert(eventName);
		}
	}
	return true;
}

bool EmitsChecker::checkIfSpecified(std::list<EmitsSpec>& specs, EventDefinition const* ev)
{
	bool ok = false;
	for (auto& spec: specs)
	{
		if (spec.events.find(ev) != spec.events.end())
		{
			spec.alreadyEmitted = true;
			ok = true;
		}
	}
	return ok;
}

EmitsChecker::EmitsChecker(BoogieContext& context) :
		m_context(context),
		m_currentScope(nullptr),
		m_currentContract(nullptr)
{ }

bool EmitsChecker::check()
{
	bool specsSatisfied = true;

	// First go through all functions and parse specs
	map<FunctionDefinition const*, list<EmitsSpec>> specified;
	for (auto fn: m_allFunctions)
		if (!collectEmitsSpecs(fn.first, specified[fn.first]))
			return false;

	// Then go through functions again and check specs
	for (auto fn: m_allFunctions)
	{
		// Check directly emitted events
		for (auto ev: m_directlyEmitted[fn.first])
		{
			if (!checkIfSpecified(specified[fn.first], ev))
			{
				m_context.reportError(fn.first, "Function possibly emits '" + ev->name() + "' without specifying");
				specsSatisfied = false;
			}
		}

		// Check specs of called functions (incl. base constructors)
		for (auto called: m_calledFuncs[fn.first])
		{
			string calledName = called->isConstructor() ? "base constructor" : called->name();

			for (auto& spec: specified[called])
			{
				for (auto ev: spec.events)
				{
					if (!checkIfSpecified(specified[fn.first], ev))
					{
						m_context.reportError(fn.first, "Function possibly emits '" + ev->name() + "' (via calling " + calledName + ") without specifying");
						specsSatisfied = false;
					}
				}
			}
		}

		// Check what called modifiers emit
		for (auto modif: fn.first->modifiers())
		{
			auto modifierDecl = dynamic_cast<ModifierDefinition const*>(modif->name()->annotation().referencedDeclaration);
			if (modifierDecl)
			{
				for (auto ev: m_directlyEmitted[modifierDecl])
				{
					if (!checkIfSpecified(specified[fn.first], ev))
					{
						m_context.reportError(fn.first, "Function possibly emits '" + ev->name() +
								"' (via modifier " + modifierDecl->name() + ") without specifying");
						specsSatisfied = false;
					}
				}
			}
			// TODO: else report unsupported case (except if it is a base constructor call)
		}

		// Finally give warnings for specified but not emitted events
		for (auto entry: specified[fn.first])
		{
			if (!entry.alreadyEmitted)
				m_context.reportWarning(fn.first, "Function specifies '" + (*entry.events.begin())->name() + "' but never emits.");
		}
	}

	return specsSatisfied;
}

bool EmitsChecker::visit(ContractDefinition const& _node)
{
	m_currentContract = &_node;
	return true;
}

bool EmitsChecker::visit(FunctionDefinition const& _node)
{
	solAssert(m_currentContract, "FunctionDefinition without ContractDefinition");

	m_currentScope = &_node;
	m_allFunctions[&_node] = m_currentContract;

	// Add base constructor calls
	if (_node.isConstructor())
		for (auto base: m_currentContract->annotation().linearizedBaseContracts)
			for (auto fn: ASTNode::filteredNodes<FunctionDefinition>(base->subNodes()))
				if (fn->isConstructor())
					m_calledFuncs[m_currentScope].insert(fn);

	return true;
}

bool EmitsChecker::visit(ModifierDefinition const& _node)
{
	m_currentScope = &_node;
	return true;
}

bool EmitsChecker::visit(EmitStatement const& _node)
{
	solAssert(m_currentScope, "EmitStatement without scope");
	EventDefinition const* eventDef = nullptr;
	if (auto id = dynamic_cast<Identifier const*>(&_node.eventCall().expression()))
		eventDef = dynamic_cast<EventDefinition const*>(id->annotation().referencedDeclaration);

	if (!eventDef)
	{
		m_context.reportError(&_node, "Unsupported emit statement, could not determine event");
		return false;
	}
	m_directlyEmitted[m_currentScope].insert(eventDef);
	return true;
}

void EmitsChecker::endVisit(ContractDefinition const&)
{
	m_currentContract = nullptr;
}

void EmitsChecker::endVisit(FunctionDefinition const&)
{
	m_currentScope = nullptr;
}

void EmitsChecker::endVisit(ModifierDefinition const&)
{
	m_currentScope = nullptr;
}

bool EmitsChecker::visit(FunctionCall const& _node)
{
	// We are not in any scope, this should not be an 'actual' function call
	// that we are interested in. E.g., conversion in state variable initializer
	if (!m_currentScope)
		return false;

	FunctionDefinition const* calledDef = nullptr;
	if (auto id = dynamic_cast<Identifier const*>(&_node.expression()))
		calledDef = dynamic_cast<FunctionDefinition const*>(id->annotation().referencedDeclaration);
	if (auto memAcc = dynamic_cast<MemberAccess const*>(&_node.expression()))
		calledDef = dynamic_cast<FunctionDefinition const*>(memAcc->annotation().referencedDeclaration);

	// TODO: handle other cases for function calls
	// TODO: if we cannot get the definition we don't give a warning yet because it would break a lot of tests

	if (calledDef)
		m_calledFuncs[m_currentScope].insert(calledDef);

	return true;
}
