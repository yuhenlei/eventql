ZBase.registerView((function() {
  var render = function() {
    var page = $.getTemplate(
        "views/settings_api_token",
        "zbase_settings_api_token_main_tpl");

    var menu = SettingsMenu();
    menu.render($(".zbase_settings_menu_sidebar", page));

    $.handleLinks(page);
    $.replaceViewport(page);

    $.httpGet("/api/v1/auth/private_api_token?allow_write=1", function(r) {
      if (r.status == 200) {
        $("input.api_token").value = JSON.parse(r.response).api_token;
      } else {
        $.fatalError();
      }
    });

    $.hideLoader();
  };

  return {
    name: "settings_api_token",
    loadView: function(params) { render(); },
    unloadView: function() {},
    handleNavigationChange: render
  };

})());
