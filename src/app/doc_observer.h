// Aseprite
// Copyright (C) 2018-2025  Igara Studio S.A.
// Copyright (C) 2001-2018  David Capello
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.

#ifndef APP_DOC_OBSERVER_H_INCLUDED
#define APP_DOC_OBSERVER_H_INCLUDED
#pragma once

namespace app {

class Doc;

class DocObserver {
public:
  virtual ~DocObserver() {}
  virtual void onFileNameChanged(Doc* doc) {}
  virtual void onGeneralUpdate(Doc* doc) {}
};

} // namespace app

#endif
