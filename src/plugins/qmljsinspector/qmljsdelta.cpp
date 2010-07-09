/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2010 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/

#include "qmljsdelta.h"
#include "qmljsclientproxy.h"
#include <qmljs/parser/qmljsast_p.h>
#include <qmljs/parser/qmljsastvisitor_p.h>

#include <typeinfo>
#include <QtCore/QDebug>

using namespace QmlJS;
using namespace QmlJS::AST;
using namespace QmlJSInspector::Internal;

UiObjectMember *ScriptBindingParser::parent(UiScriptBinding *script) const
{ return _parent.value(script); }

UiScriptBinding *ScriptBindingParser::id(UiObjectMember *parent) const
{ return _id.value(parent); }

QList<UiScriptBinding *> ScriptBindingParser::ids() const
{ return _id.values(); }

QString ScriptBindingParser::header(UiObjectMember *member) const
{
    if (member) {
        if (UiObjectDefinition *def = cast<UiObjectDefinition *>(member)) {
            const int begin = def->firstSourceLocation().begin();
            const int end = def->initializer->lbraceToken.begin();
            return doc->source().mid(begin, end - begin);
        } else if (UiObjectBinding *binding = cast<UiObjectBinding *>(member)) {
            const int begin = binding->firstSourceLocation().begin();
            const int end = binding->initializer->lbraceToken.begin();
            return doc->source().mid(begin, end - begin);
        }
    }

    return QString();
}

QString ScriptBindingParser::scriptCode(UiScriptBinding *script) const
{
    if (script) {
        const int begin = script->statement->firstSourceLocation().begin();
        const int end = script->statement->lastSourceLocation().end();
        return doc->source().mid(begin, end - begin);
    }

    return QString();
}


QString ScriptBindingParser::methodName(UiSourceElement *source) const
{
    if (source) {
        if (FunctionDeclaration *declaration = cast<FunctionDeclaration*>(source->sourceElement)) {
            return declaration->name->asString();
        }
    }
    return QString();
}

QString ScriptBindingParser::methodCode(UiSourceElement *source) const
{
    if (source) {
        if (FunctionDeclaration *declaration = cast<FunctionDeclaration*>(source->sourceElement)) {
            const int begin = declaration->lbraceToken.begin() + 1;
            const int end = declaration->rbraceToken.end() - 1;
            return doc->source().mid(begin, end - begin);
        }
    }
    return QString();
}

static bool isLiteralValue(ExpressionNode *expr)
{
    if (cast<NumericLiteral*>(expr))
        return true;
    else if (cast<StringLiteral*>(expr))
        return true;
    else if (UnaryPlusExpression *plusExpr = cast<UnaryPlusExpression*>(expr))
        return isLiteralValue(plusExpr->expression);
    else if (UnaryMinusExpression *minusExpr = cast<UnaryMinusExpression*>(expr))
        return isLiteralValue(minusExpr->expression);
    else if (cast<TrueLiteral*>(expr))
        return true;
    else if (cast<FalseLiteral*>(expr))
        return true;
    else
        return false;
}

static inline bool isLiteralValue(UiScriptBinding *script)
{
    if (!script || !script->statement)
        return false;

    ExpressionStatement *exprStmt = cast<ExpressionStatement *>(script->statement);
    if (exprStmt)
        return isLiteralValue(exprStmt->expression);
    else
        return false;
}

static inline QString stripQuotes(const QString &str)
{
    if ((str.startsWith(QLatin1Char('"')) && str.endsWith(QLatin1Char('"')))
            || (str.startsWith(QLatin1Char('\'')) && str.endsWith(QLatin1Char('\''))))
        return str.mid(1, str.length() - 2);

    return str;
}

static inline QString deEscape(const QString &value)
{
    QString result = value;

    result.replace(QLatin1String("\\\\"), QLatin1String("\\"));
    result.replace(QLatin1String("\\\""), QLatin1String("\""));
    result.replace(QLatin1String("\\\t"), QLatin1String("\t"));
    result.replace(QLatin1String("\\\r"), QLatin1String("\\\r"));
    result.replace(QLatin1String("\\\n"), QLatin1String("\n"));

    return result;
}

static QString cleanExpression(const QString &expression, UiScriptBinding *scriptBinding)
{
    QString trimmedExpression = expression.trimmed();

    if (ExpressionStatement *expStatement = cast<ExpressionStatement*>(scriptBinding->statement)) {
        if (expStatement->semicolonToken.isValid())
            trimmedExpression.chop(1);
    }

    return deEscape(stripQuotes(trimmedExpression));
}

static QVariant castToLiteral(const QString &expression, UiScriptBinding *scriptBinding)
{
    const QString cleanedValue = cleanExpression(expression, scriptBinding);
    QVariant castedExpression;

    ExpressionStatement *expStatement = cast<ExpressionStatement*>(scriptBinding->statement);

    switch(expStatement->expression->kind) {
    case Node::Kind_NumericLiteral:
    case Node::Kind_UnaryPlusExpression:
    case Node::Kind_UnaryMinusExpression:
        castedExpression = QVariant(cleanedValue).toReal();
        break;
    case Node::Kind_StringLiteral:
        castedExpression = QVariant(cleanedValue).toString();
        break;
    case Node::Kind_TrueLiteral:
    case Node::Kind_FalseLiteral:
        castedExpression = QVariant(cleanedValue).toBool();
        break;
    default:
        castedExpression = cleanedValue;
        break;
    }

    return castedExpression;
}

ScriptBindingParser::ScriptBindingParser(QmlJS::Document::Ptr doc,
                                         const QList<QDeclarativeDebugObjectReference> &objectReferences)
    : doc(doc), objectReferences(objectReferences), m_searchElementOffset(-1)
{

}

void ScriptBindingParser::process()
{
    if (!doc.isNull() && doc->qmlProgram())
        doc->qmlProgram()->accept(this);
}

QDeclarativeDebugObjectReference ScriptBindingParser::objectReferenceForOffset(unsigned int offset)
{
    m_searchElementOffset = offset;
    if (!doc.isNull() && doc->qmlProgram())
        doc->qmlProgram()->accept(this);

    return m_foundObjectReference;
}

QDeclarativeDebugObjectReference ScriptBindingParser::objectReference(const QString &id) const
{
    foreach (const QDeclarativeDebugObjectReference &ref, objectReferences) {
        if (ref.idString() == id)
            return ref;
    }

    return QDeclarativeDebugObjectReference();
}


QDeclarativeDebugObjectReference ScriptBindingParser::objectReferenceForPosition(const QUrl &url, int line, int col) const
{
    foreach (const QDeclarativeDebugObjectReference &ref, objectReferences) {
        if (ref.source().lineNumber() == line
         && ref.source().columnNumber() == col
         && ref.source().url() == url)
        {
            return ref;
        }
    }

    return QDeclarativeDebugObjectReference();
}

bool ScriptBindingParser::visit(UiObjectDefinition *ast)
{
    objectStack.append(ast);
    return true;
}

void ScriptBindingParser::endVisit(UiObjectDefinition *)
{
    objectStack.removeLast();
}

bool ScriptBindingParser::visit(UiObjectBinding *ast)
{
    objectStack.append(ast);
    return true;
}

void ScriptBindingParser::endVisit(UiObjectBinding *)
{
    objectStack.removeLast();
}

bool ScriptBindingParser::visit(UiScriptBinding *ast)
{
    scripts.append(ast);
    _parent[ast] = objectStack.back();

    if (ast->qualifiedId && ast->qualifiedId->name && ! ast->qualifiedId->next) {
        const QString bindingName = ast->qualifiedId->name->asString();

        if (bindingName == QLatin1String("id")) {
            _id[objectStack.back()] = ast;

            if (ExpressionStatement *s = cast<ExpressionStatement *>(ast->statement)) {
                if (IdentifierExpression *id = cast<IdentifierExpression *>(s->expression)) {
                    if (id->name) {
                        _idBindings.insert(ast, objectReference(id->name->asString()));

                        if (parent(ast)->firstSourceLocation().offset == m_searchElementOffset)
                            m_foundObjectReference = objectReference(id->name->asString());
                    }
                }
            }
        }
    }

    return true;
}

QDeclarativeDebugObjectReference ScriptBindingParser::objectReferenceForScriptBinding(UiScriptBinding *binding) const
{
    return _idBindings.value(binding);
}

// Delta

static QString propertyName(UiQualifiedId *id)
{
    QString s;

    for (; id; id = id->next) {
        if (! id->name)
            return QString();

        s += id->name->asString();

        if (id->next)
            s += QLatin1Char('.');
    }

    return s;
}

QDeclarativeDebugObjectReference Delta::objectReferenceForUiObject(const ScriptBindingParser &bindingParser, UiObjectMember *object)
{
    if (UiScriptBinding *idBinding = bindingParser.id(object)) {
        if (ExpressionStatement *s = cast<ExpressionStatement *>(idBinding->statement)) {
            if (IdentifierExpression *idExpr = cast<IdentifierExpression *>(s->expression)) {
                const QString idString = idExpr->name->asString();

                const QList<QDeclarativeDebugObjectReference> refs = ClientProxy::instance()->objectReferences(_url);
                foreach (const QDeclarativeDebugObjectReference &ref, refs) {
                    if (ref.idString() == idString)
                        return ref;
                }
            }
        }
    }
    return QDeclarativeDebugObjectReference();
}


void Delta::operator()(Document::Ptr doc, Document::Ptr previousDoc)
{
    _doc = doc;
    _previousDoc = previousDoc;
    _changes.clear();

    _url = QUrl::fromLocalFile(doc->fileName());
    const QList<QDeclarativeDebugObjectReference> references = ClientProxy::instance()->objectReferences(_url);

    ScriptBindingParser bindingParser(doc, references);
    bindingParser.process();

    ScriptBindingParser previousBindingParser(previousDoc, references);
    previousBindingParser.process();

    QHash<UiObjectMember *, UiObjectMember *> preservedObjects;

    foreach (UiScriptBinding *id, bindingParser.ids()) {
        UiObjectMember *parent = bindingParser.parent(id);
        const QString idCode = bindingParser.scriptCode(id);

        foreach (UiScriptBinding *otherId, previousBindingParser.ids()) {
            const QString otherIdCode = previousBindingParser.scriptCode(otherId);

            if (idCode == otherIdCode) {
                preservedObjects.insert(parent, previousBindingParser.parent(otherId));
            }
        }
    }

    QHashIterator<UiObjectMember *, UiObjectMember *> it(preservedObjects);
    while (it.hasNext()) {
        it.next();

        UiObjectMember *object = it.key();
        UiObjectMember *previousObject = it.value();

        for (UiObjectMemberList *objectMemberIt = objectMembers(object); objectMemberIt; objectMemberIt = objectMemberIt->next) {
            if (UiScriptBinding *script = cast<UiScriptBinding *>(objectMemberIt->member)) {
                for (UiObjectMemberList *previousObjectMemberIt = objectMembers(previousObject); previousObjectMemberIt; previousObjectMemberIt = previousObjectMemberIt->next) {
                    if (UiScriptBinding *previousScript = cast<UiScriptBinding *>(previousObjectMemberIt->member)) {
                        if (compare(script->qualifiedId, previousScript->qualifiedId)) {
                            const QString scriptCode = bindingParser.scriptCode(script);
                            const QString previousScriptCode = previousBindingParser.scriptCode(previousScript);

                            if (scriptCode != previousScriptCode) {
                                const QString property = propertyName(script->qualifiedId);

                                QDeclarativeDebugObjectReference ref = objectReferenceForUiObject(bindingParser, object);
                                if (ref.debugId() != -1)
                                    updateScriptBinding(ref, script, property, scriptCode);
                            }
                        }
                    }
                }
            } else if (UiSourceElement *uiSource = cast<UiSourceElement*>(objectMemberIt->member)) {

                for (UiObjectMemberList *previousObjectMemberIt = objectMembers(previousObject);
                     previousObjectMemberIt; previousObjectMemberIt = previousObjectMemberIt->next)
                {
                    if (UiSourceElement *previousSource = cast<UiSourceElement*>(previousObjectMemberIt->member)) {
                        if (compare(uiSource, previousSource))
                        {
                            const QString methodCode = bindingParser.methodCode(uiSource);
                            const QString previousMethodCode = previousBindingParser.methodCode(previousSource);

                            if (methodCode != previousMethodCode) {
                                const QString methodName = bindingParser.methodName(uiSource);
                                QDeclarativeDebugObjectReference ref = objectReferenceForUiObject(bindingParser, object);
                                if (ref.debugId() != -1)
                                    updateMethodBody(ref, script, methodName, methodCode);
                            }
                        }
                    }
                }

            }
        }

    }
}

void Delta::updateMethodBody(const QDeclarativeDebugObjectReference &objectReference,
                               UiScriptBinding *scriptBinding,
                               const QString &methodName,
                               const QString &methodBody)
{
    Change change;
    change.script = scriptBinding;
    change.ref = objectReference;
    change.isLiteral = false;
    _changes.append(change);

    ClientProxy::instance()->setMethodBodyForObject(objectReference.debugId(), methodName, methodBody); // ### remove
}

void Delta::updateScriptBinding(const QDeclarativeDebugObjectReference &objectReference,
                                UiScriptBinding *scriptBinding,
                                const QString &propertyName,
                                const QString &scriptCode)
{
    QVariant expr = scriptCode;

    const bool isLiteral = isLiteralValue(scriptBinding);
    if (isLiteral)
        expr = castToLiteral(scriptCode, scriptBinding);

    Change change;
    change.script = scriptBinding;
    change.ref = objectReference;
    change.isLiteral = isLiteral;
    _changes.append(change);

    ClientProxy::instance()->setBindingForObject(objectReference.debugId(), propertyName, expr, isLiteral); // ### remove
}

bool Delta::compare(UiQualifiedId *id, UiQualifiedId *other)
{
    if (id == other)
        return true;

    else if (id && other) {
        if (id->name && other->name) {
            if (id->name->asString() == other->name->asString())
                return compare(id->next, other->next);
        }
    }

    return false;
}

bool Delta::compare(UiSourceElement *source, UiSourceElement *other)
{
    if (source == other)
        return true;

    else if (source && other) {
        if (source->sourceElement && other->sourceElement) {
            FunctionDeclaration *decl = cast<FunctionDeclaration*>(source->sourceElement);
            FunctionDeclaration *otherDecl = cast<FunctionDeclaration*>(other->sourceElement);
            if (decl && otherDecl
                && decl->name && otherDecl->name
                && decl->name->asString() == otherDecl->name->asString())
            {
                    return true;
            }
        }
    }

    return false;
}

UiObjectMemberList *Delta::objectMembers(UiObjectMember *object)
{
    if (UiObjectDefinition *def = cast<UiObjectDefinition *>(object))
        return def->initializer->members;
    else if (UiObjectBinding *binding = cast<UiObjectBinding *>(object))
        return binding->initializer->members;

    return 0;
}

Document::Ptr Delta::document() const
{
    return _doc;
}

Document::Ptr Delta::previousDocument() const
{
    return _previousDoc;
}

QList<Delta::Change> Delta::changes() const
{
    return _changes;
}
