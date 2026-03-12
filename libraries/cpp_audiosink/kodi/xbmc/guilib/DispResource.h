#pragma once

class IDispResource
{
public:
  virtual ~IDispResource() = default;

  virtual void OnLostDisplay() {}
  virtual void OnResetDisplay() {}
  virtual void OnAppFocusChange(bool /* focus */) {}
};
