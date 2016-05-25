/*
 * Copyright (C) 2013-2016 Daniel Nicoletti <dantti12@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "clearsilver_p.h"

#include "context.h"
#include "action.h"
#include "response.h"

#include <QString>
#include <QFile>
#include <QtCore/QLoggingCategory>

Q_LOGGING_CATEGORY(CUTELYST_CLEARSILVER, "cutelyst.clearsilver")

using namespace Cutelyst;

ClearSilver::ClearSilver(QObject *parent) : View(parent)
  , d_ptr(new ClearSilverPrivate)
{
}

ClearSilver::~ClearSilver()
{
    delete d_ptr;
}

QStringList ClearSilver::includePaths() const
{
    Q_D(const ClearSilver);
    return d->includePaths;
}

void ClearSilver::setIncludePaths(const QStringList &paths)
{
    Q_D(ClearSilver);
    d->includePaths = paths;
}

QString ClearSilver::templateExtension() const
{
    Q_D(const ClearSilver);
    return d->extension;
}

void ClearSilver::setTemplateExtension(const QString &extension)
{
    Q_D(ClearSilver);
    d->extension = extension;
}

QString ClearSilver::wrapper() const
{
    Q_D(const ClearSilver);
    return d->wrapper;
}

void ClearSilver::setWrapper(const QString &name)
{
    Q_D(ClearSilver);
    d->wrapper = name;
}

NEOERR* cutelyst_render(void *user, char *data)
{
    QByteArray *body = static_cast<QByteArray*>(user);
    if (body) {
        body->append(data);
    }
//    qDebug() << "_render" << body << data;
    return 0;
}

QByteArray ClearSilver::render(Context *c) const
{
    Q_D(const ClearSilver);

    const QVariantHash &stash = c->stash();
    QString templateFile = stash.value(QStringLiteral("template")).toString();
    if (templateFile.isEmpty()) {
        if (c->action() && !c->action()->reverse().isEmpty()) {
            templateFile = c->action()->reverse() + d->extension;
        }

        if (templateFile.isEmpty()) {
            c->error(QStringLiteral("Cannot render template, template name or template stash key not defined"));
            return QByteArray();
        }
    }

    qCDebug(CUTELYST_CLEARSILVER) << "Rendering template" <<templateFile;
    QByteArray output;
    if (!d->render(c, templateFile, stash, output)) {
        return QByteArray();
    }

    if (!d->wrapper.isEmpty()) {
        QString wrapperFile = d->wrapper;

        QVariantHash data = stash;
        data.insert(QStringLiteral("content"), output);

        if (!d->render(c, wrapperFile, data, output)) {
            return QByteArray();
        }
    }

    return output;
}

NEOERR* findFile(void *c, HDF *hdf, const char *filename, char **contents)
{
    ClearSilverPrivate *priv = static_cast<ClearSilverPrivate*>(c);
    if (!priv) {
        return nerr_raise(NERR_NOMEM, "Cound not cast ClearSilverPrivate");
    }

    Q_FOREACH (const QString &includePath, priv->includePaths) {
        QFile file(includePath + QLatin1Char('/') + QString::fromLatin1(filename));

        if (file.exists()) {
            if (!file.open(QFile::ReadOnly)) {
                return nerr_raise(NERR_IO, "Cound not open file: %s", file.errorString().toLatin1().data());
            }

            *contents = qstrdup(file.readAll().constData());
            qCDebug(CUTELYST_CLEARSILVER) << "Rendering template:" << file.fileName();
            return 0;
        }
    }

    return nerr_raise(NERR_NOT_FOUND, "Cound not find file: %s", filename);
}

bool ClearSilverPrivate::render(Context *c, const QString &filename, const QVariantHash &stash, QByteArray &output) const
{
    HDF *hdf = hdfForStash(c, stash);
    CSPARSE *cs;
    NEOERR *error;

    error = cs_init(&cs, hdf);
    if (error) {
        STRING *msg = new STRING;
        string_init(msg);
        nerr_error_traceback(error, msg);
        QString errorMsg;
        errorMsg = QStringLiteral("Failed to init ClearSilver:\n+1").arg(QString::fromLatin1(msg->buf, msg->len));
        renderError(c, errorMsg);

        hdf_destroy(&hdf);
        nerr_ignore(&error);
        return false;
    }

    cs_register_fileload(cs, const_cast<ClearSilverPrivate*>(this), findFile);

    error = cs_parse_file(cs, filename.toLatin1().data());
    if (error) {
        STRING *msg = new STRING;
        string_init(msg);
        nerr_error_traceback(error, msg);
        QString errorMsg;
        errorMsg = QStringLiteral("Failed to parse template file: +1\n+2").arg(filename, QString::fromLatin1(msg->buf, msg->len));
        renderError(c, errorMsg);

        nerr_log_error(error);
        hdf_destroy(&hdf);
        nerr_ignore(&error);
        return false;
    }

    cs_render(cs, &output, cutelyst_render);

    cs_destroy(&cs);
    hdf_destroy(&hdf);

    return true;
}

void ClearSilverPrivate::renderError(Context *c, const QString &error) const
{
    c->error(error);
    c->res()->body() = error.toUtf8();
}

HDF *ClearSilverPrivate::hdfForStash(Context *c, const QVariantHash &stash) const
{
    HDF *hdf = 0;
    hdf_init(&hdf);

    serializeHash(hdf, stash);

    const QMetaObject *meta = c->metaObject();
    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        QString name = QLatin1String("c.") + QString::fromLatin1(prop.name());
        QVariant value = prop.read(c);
        serializeVariant(hdf, value, name);
    }
    return hdf;
}

void ClearSilverPrivate::serializeHash(HDF *hdf, const QVariantHash &hash, const QString &prefix) const
{
    QString _prefix;
    if (!prefix.isNull()) {
        _prefix = prefix + QLatin1Char('.');
    }

    auto it = hash.constBegin();
    while (it != hash.constEnd()) {
        serializeVariant(hdf, it.value(), _prefix + it.key());
        ++it;
    }
}

void ClearSilverPrivate::serializeMap(HDF *hdf, const QVariantMap &map, const QString &prefix) const
{
    QString _prefix;
    if (!prefix.isNull()) {
        _prefix = prefix + QLatin1Char('.');
    }

    auto it = map.constBegin();
    while (it != map.constEnd()) {
        serializeVariant(hdf, it.value(), _prefix + it.key());
        ++it;
    }
}

void ClearSilverPrivate::serializeVariant(HDF *hdf, const QVariant &value, const QString &key) const
{
//    qDebug() << key;

    switch (value.type()) {
    case QMetaType::QString:
        hdf_set_value(hdf, key.toLatin1().data(), value.toString().toLatin1().data());
        break;
    case QMetaType::Int:
        hdf_set_int_value(hdf, key.toLatin1().data(), value.toInt());
        break;
    case QMetaType::QVariantHash:
        serializeHash(hdf, value.toHash(), key);
        break;
    case QMetaType::QVariantMap:
        serializeMap(hdf, value.toMap(), key);
        break;
    default:
        if (value.canConvert(QMetaType::QString)) {
            hdf_set_value(hdf, key.toLatin1().data(), value.toString().toLatin1().data());
        }
        break;
    }
}

#include "moc_clearsilver.cpp"
