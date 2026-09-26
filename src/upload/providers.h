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
// WebDAV upload plus a public share link, with Login Flow v2 sign-in.
const Provider *nextcloudProvider();
// Dropbox with a PKCE browser sign-in and the user's own app key.
const Provider *dropboxProvider();
// An asset in an Immich library, optionally shared.
const Provider *immichProvider();
// A self-hosted XBackBone instance.
const Provider *xbackboneProvider();
// Anonymous Imgur uploads with the user's own client ID.
const Provider *imgurProvider();

// The anonymous hosts offered out of the box, as .sxcu documents.
QList<QByteArray> builtInDefinitions();

}
