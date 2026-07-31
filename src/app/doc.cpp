// Aseprite
// Copyright (C) 2018-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/doc.h"

#include "app/doc_observer.h"
#include "app/file/file.h"

namespace app {

Doc::Doc(doc::Sprite* sprite)
  : m_sprite(sprite)
  , m_filename("")
  , m_undo(this)
{
  if (m_sprite)
    m_sprite->setDocument(this);
}

Doc::~Doc()
{
  delete m_sprite;
}

void Doc::setFilename(const std::string& filename)
{
  m_filename = filename;
  notifyFileNameChanged();
}

std::string Doc::name() const
{
  if (m_filename.empty())
    return "Untitled";
  return base::get_file_name(m_filename);
}

void Doc::notifyFileNameChanged()
{
  for (auto observer : m_observers)
    observer->onFileNameChanged(this);
}

void Doc::notifyGeneralUpdate()
{
  for (auto observer : m_observers)
    observer->onGeneralUpdate(this);
}

} // namespace app
