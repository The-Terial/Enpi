# Security Note

The local project contains a PHP/MySQL web application under `Gestion_Energetica/`. That application was **not copied** into this reproducibility package because the local configuration file contains real database credentials.

Before uploading any platform source code to a public repository:

1. Rotate any exposed database credentials.
2. Remove `app/config.php`.
3. Add a sanitized `app/config.sample.php`.
4. Check the repository with a secret scanner before publishing.

This reproducibility package intentionally focuses on data, analysis scripts, manuscript artifacts, and figures.
