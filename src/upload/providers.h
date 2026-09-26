#pragma once

#include <QByteArray>
#include <QList>

// The providers omacut ships, one per source file.
namespace upload {

class Provider;

// A ShareX custom uploader, given as .sxcu JSON in the "definition" setting.
const Provider *sxcuProvider();

// Amazon S3 and the S3-compatible stores.
const Provider *s3Provider();

// The anonymous hosts offered out of the box, as .sxcu documents.
QList<QByteArray> builtInDefinitions();

}
