// Aseprite
// Copyright (C) 2018-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef APP_DOC_H_INCLUDED
#define APP_DOC_H_INCLUDED
#pragma once

#include "app/doc_undo.h"
#include "doc/sprite.h"
#include "obs/observable.h"

#include <string>

namespace app {

class DocObserver;

class Doc : public obs::observable<DocObserver> {
public:
  Doc(doc::Sprite* sprite = nullptr);
  ~Doc();

  doc::Sprite* sprite() const { return m_sprite; }
  DocUndo* undo() { return &m_undo; }

  const std::string& filename() const { return m_filename; }
  void setFilename(const std::string& filename);

  std::string name() const;

  bool isModified() const { return m_undo.canUndo(); }
  bool isAssociatedToFile() const { return !m_filename.empty(); }

  void notifyFileNameChanged();
  void notifyGeneralUpdate();

private:
  doc::Sprite* m_sprite{nullptr};
  std::string m_filename;
  DocUndo m_undo;
};

} // namespace app

#endif
